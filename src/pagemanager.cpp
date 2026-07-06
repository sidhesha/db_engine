#include "pagemanager.hpp"
#include <cstring>

PageManager::PageManager(const std::string& fname)
    : buffer_pool(fname) {}

PageManager::~PageManager() {
    buffer_pool.flush();
}

Page PageManager::readPage(int page_id) {
    Page& cached = buffer_pool.fetchPage(page_id);
    std::vector<char> data = cached.serialize();
    buffer_pool.unpinPage(page_id, false);
    return Page::deserialize(data);
}

void PageManager::writePage(Page& page) {
    std::vector<char> data = page.serialize();
    Page& cached = buffer_pool.fetchPage(page.getPageId());
    cached = Page::deserialize(data);
    buffer_pool.unpinPage(page.getPageId(), true);
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
