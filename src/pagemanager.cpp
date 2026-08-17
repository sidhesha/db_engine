#include "db_engine/pagemanager.hpp"
#include <cstring>

PageManager::PageManager(const std::string& fname, WALWriter& wal, TransactionManager& txns,
                          uint32_t table_id)
    : buffer_pool(fname, wal, txns, table_id) {}

PageManager::~PageManager() {
    buffer_pool.flush();
}

Page PageManager::readPage(int page_id) {
    Page& cached = buffer_pool.fetchPage(page_id);
    std::vector<char> data = cached.serialize();
    buffer_pool.unpinPage(page_id, false);
    return Page::deserialize(data);
}

void PageManager::writePage(Page& page, uint64_t txn_id) {
    std::vector<char> data = page.serialize();
    Page& cached = buffer_pool.fetchPage(page.getPageId());
    cached = Page::deserialize(data);
    buffer_pool.unpinPage(page.getPageId(), true, txn_id);
}

int PageManager::allocatePage() {
    return buffer_pool.allocatePage();
}

int PageManager::getNextPageId() {
    return buffer_pool.getNextPageId();
}

void PageManager::flush() {
    buffer_pool.flush();
}
