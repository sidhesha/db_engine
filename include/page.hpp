#pragma once

#include <vector>
#include <string>
#include <cstdint>

const int PAGE_SIZE = 4096;         // Fixed size of each page
const int PAGE_HEADER_SIZE = 8;     // 4 bytes page_id + 2 bytes num_slots + 2 bytes free_space_offset
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
    static Page deserialize(const char* raw);

    int insertRecord(const std::string& data); // returns slot_id or -1
    std::string readRecord(int slot_id) const;
    bool deleteRecord(int slot_id);

    char* serialize();
    uint32_t getPageId() const;
    int getFreeSpace() const;
    int getNumSlots() const;

private:
    uint32_t page_id;
    uint16_t num_slots;
    uint16_t free_space_offset;

    std::vector<SlotEntry> slot_directory;
    std::vector<char> data; // full 4096-byte page data

    void updateSlotDirectory();  // sync slot_directory vector to 'data'
    void updateHeaderToData(); // sync header variable to 'data'
};
