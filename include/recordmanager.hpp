#pragma once

#include "record.hpp"
#include "pagemanager.hpp"
#include "constants.hpp"

class RecordManager {
public:
    RecordManager(PageManager& page_manager);

    // txn_id == 0 (default): auto-commit, same as before Phase 5. A real
    // txn_id groups this write under a caller-owned transaction -- see
    // BufferPool::unpinPage/PageManager::writePage.
    RID insertRecord(const Record& record, uint64_t txn_id = 0);
    Record readRecord(const RID& rid);
    bool deleteRecord(const RID& rid, uint64_t txn_id = 0);

private:
    PageManager& page_manager;
};
