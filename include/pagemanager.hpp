#pragma once

#include "page.hpp"
#include "bufferpool.hpp"
#include "wal.hpp"
#include <string>

class PageManager {
public:
    PageManager(const std::string& filename, WALWriter& wal, TransactionManager& txns);
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
