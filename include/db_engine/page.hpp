#pragma once

#include <vector>
#include "db_engine/constants.hpp"
#include <cstdint>

// const int PAGE_SIZE = 4096;         // Fixed size of each page
const int PAGE_HEADER_SIZE = 16;    // 4 bytes page_id + 2 bytes num_slots + 2 bytes free_space_offset + 8 bytes lsn
const int SLOT_ENTRY_SIZE = 5;      // 2 bytes offset + 2 bytes length + 1 byte is_active

// Each record in a page is tracked by a slot
struct SlotEntry {
    uint16_t offset;     // Offset to actual record in data section
    uint16_t length;     // Length of the record
    uint8_t is_active;   // 1 if record is valid, 0 if deleted
};

// Main Page class
class Page {
public:
    explicit Page(uint32_t page_id);
    static Page deserialize(const std::vector<char> raw);

    int insertRecord(const std::vector<char> record); // returns slot_id or -1
    std::vector<char> readRecord(int slot_id) const;
    bool deleteRecord(int slot_id);

    // In-place overwrite of `patch.size()` bytes starting at `offset`
    // within an existing slot's record bytes -- the slot's length is
    // never touched, so this can't disturb any other slot's layout.
    // Used by MVCC (Phase 5) to stamp delete_txn_id on an existing row
    // version (see Record::DELETE_TXN_ID_OFFSET) without rewriting the
    // whole record.
    void patchBytes(int slot_id, size_t offset, const std::vector<char>& patch);

    std::vector<char> serialize();
    uint32_t getPageId() const;
    int getFreeSpace() const;
    int getNumSlots() const; // 1 indexed

    // ARIES page LSN: the LSN of the last WAL record applied to this
    // page. Recovery's redo pass only reapplies a record whose LSN is
    // greater than this, so redo is idempotent.
    uint64_t getLSN() const;
    void setLSN(uint64_t lsn);

private:
    uint32_t page_id;
    uint16_t num_slots;
    uint16_t free_space_offset;
    uint64_t lsn = 0;

    std::vector<SlotEntry> slot_directory;
    std::vector<char> data; // full 4096-byte page data

    void updateSlotDirectory();  // sync slot_directory vector to 'data'
    void updateHeaderToData(); // sync header variable to 'data'
};
