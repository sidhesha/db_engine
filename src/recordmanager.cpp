#include "recordmanager.hpp"

RecordManager::RecordManager(PageManager &page_manager): page_manager(page_manager){};

RID RecordManager::insertRecord(const Record& record) {
    std::vector<char> serialized = record.serialize();

    // Try inserting into an existing page
    // TODO: could optimize this thing
    for (int i = 0; i < page_manager.getNextPageId(); ++i) {
        Page page = page_manager.readPage(i);
        int slot_id = page.insertRecord(serialized);
        if (slot_id != -1) {
            page_manager.writePage(page);
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

    page_manager.writePage(new_page);
    return RID{new_page_id, slot_id};
}


Record RecordManager::readRecord(const RID& rid) {
    Page page = page_manager.readPage(rid.page_id);
    std::vector<char> data = page.readRecord(rid.slot_id);
    return Record::deserialize(data);
}

bool RecordManager::deleteRecord(const RID& rid) {
    Page page = page_manager.readPage(rid.page_id);
    bool deleted = page.deleteRecord(rid.slot_id);  // marks slot invalid
    page_manager.writePage(page);    // persist the change
    return deleted;
}
