#include "recordmanager.hpp"
#include <cstring>

namespace {
std::vector<char> encodeTxnIdPatch(uint64_t txn_id) {
    std::vector<char> patch(sizeof(txn_id));
    std::memcpy(patch.data(), &txn_id, sizeof(txn_id));
    return patch;
}
}  // namespace

RecordManager::RecordManager(PageManager &page_manager): page_manager(page_manager){};

RID RecordManager::insertRecord(const Record& record, uint64_t txn_id) {
    std::lock_guard<std::mutex> lock(io_mutex);
    return insertRecordLocked(record, txn_id);
}

RID RecordManager::insertRecordLocked(const Record& record, uint64_t txn_id) {
    std::vector<char> serialized = record.serialize();

    // Try inserting into an existing page
    // TODO: could optimize this thing
    for (int i = 0; i < page_manager.getNextPageId(); ++i) {
        Page page = page_manager.readPage(i);
        int slot_id = page.insertRecord(serialized);
        if (slot_id != -1) {
            page_manager.writePage(page, txn_id);
            return RID{i, slot_id};
        }
    }

    // No space: allocate new page and write it
    int new_page_id = page_manager.allocatePage();
    Page new_page(new_page_id);
    int slot_id = new_page.insertRecord(serialized);
    if (slot_id == -1) {
        throw std::runtime_error("Record too large to fit in a page");
    }

    page_manager.writePage(new_page, txn_id);
    return RID{new_page_id, slot_id};
}


Record RecordManager::readRecord(const RID& rid) {
    std::lock_guard<std::mutex> lock(io_mutex);
    Page page = page_manager.readPage(rid.page_id);
    std::vector<char> data = page.readRecord(rid.slot_id);
    return Record::deserialize(data);
}

bool RecordManager::deleteRecord(const RID& rid, uint64_t txn_id) {
    std::lock_guard<std::mutex> lock(io_mutex);
    Page page = page_manager.readPage(rid.page_id);
    bool deleted = page.deleteRecord(rid.slot_id);  // marks slot invalid
    page_manager.writePage(page, txn_id);    // persist the change
    return deleted;
}

std::optional<RID> RecordManager::updateRecord(const RID& old_rid,
                                                const std::vector<std::string>& new_fields,
                                                uint64_t txn_id) {
    std::lock_guard<std::mutex> lock(io_mutex);
    Page old_page = page_manager.readPage(old_rid.page_id);
    Record old_record = Record::deserialize(old_page.readRecord(old_rid.slot_id));
    if (old_record.getDeleteTxnId() != 0) {
        return std::nullopt;  // already deleted -- nothing live to update
    }

    old_page.patchBytes(old_rid.slot_id, Record::DELETE_TXN_ID_OFFSET, encodeTxnIdPatch(txn_id));
    page_manager.writePage(old_page, txn_id);

    Record new_record(new_fields, txn_id, old_rid);
    return insertRecordLocked(new_record, txn_id);
}

bool RecordManager::markDeleted(const RID& rid, uint64_t txn_id) {
    std::lock_guard<std::mutex> lock(io_mutex);
    Page page = page_manager.readPage(rid.page_id);
    Record record = Record::deserialize(page.readRecord(rid.slot_id));
    if (record.getDeleteTxnId() != 0) {
        return false;  // already deleted
    }

    page.patchBytes(rid.slot_id, Record::DELETE_TXN_ID_OFFSET, encodeTxnIdPatch(txn_id));
    page_manager.writePage(page, txn_id);
    return true;
}
