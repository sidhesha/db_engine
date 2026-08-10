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
    void writePage(Page& page);
    int allocatePage();
    int getNextPageId();
    void flush();

private:
    BufferPool buffer_pool;
};
