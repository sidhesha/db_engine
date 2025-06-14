#include "pagemanager.hpp"
#include <iostream>
#include <filesystem>
#include <cstring>

PageManager::PageManager(const std::string& fname): filename(fname), next_page_id(0) {
    openFile();

    // Set next_page_id based on file size
    std::filesystem::path path(filename);
    if (std::filesystem::exists(path)) {
        std::uintmax_t filesize = std::filesystem::file_size(path);
        next_page_id = filesize / PAGE_SIZE;
    }
}

PageManager::~PageManager() {
    flush();
    file.close();
}

void PageManager::openFile() {
    file.open(filename, std::ios::in | std::ios::out | std::ios::binary);

    // If file doesn't exist, create it
    if (!file.is_open()) {
        file.clear();
        file.open(filename, std::ios::out | std::ios::binary);
        file.close();
        file.open(filename, std::ios::in | std::ios::out | std::ios::binary);
    }

    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }
}


Page PageManager::readPage(int page_id) {
    Page page(page_id);
    char buffer[PAGE_SIZE];

    file.seekg(page_id * PAGE_SIZE, std::ios::beg);
    file.read(buffer, PAGE_SIZE);

    if (!file) {
        throw std::runtime_error("Failed to read page " + std::to_string(page_id));
    }

    page = page.deserialize(buffer);
    return page;
}

void PageManager::writePage(Page& page) {
    std::vector<char> buffer = page.serialize();

    file.seekp(page.getPageId() * PAGE_SIZE, std::ios::beg);
    file.write(buffer.data(), PAGE_SIZE);

    if (!file) {
        throw std::runtime_error("Failed to write page " + std::to_string(page.getPageId()));
    }
}

int PageManager::allocatePage() {
    int page_id = next_page_id++;
    Page new_page(page_id);
    writePage(new_page); // Write an empty page to disk
    return page_id;
}

void PageManager::flush() {
    file.flush();
}

