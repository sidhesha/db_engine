#pragma once

#include "db_engine/page.hpp"
#include "db_engine/bufferpool.hpp"
#include "db_engine/wal.hpp"
#include <string>

class PageManager {
public:
    PageManager(const std::string& filename, WALWriter& wal, TransactionManager& txns,
                uint32_t table_id = 0);
    ~PageManager();

    Page readPage(int page_id);
    // txn_id == 0 (default): auto-commit, same as before Phase 5. A real
    // txn_id groups this write under a caller-owned, multi-statement
    // transaction -- see BufferPool::unpinPage.
    void writePage(Page& page, uint64_t txn_id = 0);
    int allocatePage();
    int getNextPageId();
    void flush();

private:
    BufferPool buffer_pool;
};
