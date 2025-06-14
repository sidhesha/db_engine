#pragma once

#include "page.hpp"
#include <fstream>
#include <string>

class PageManager {
public:
    PageManager(const std::string& filename);
    ~PageManager();

    Page readPage(int page_id);
    void writePage(Page& page);
    int allocatePage();
    void flush();

private:
    std::fstream file;
    std::string filename;
    int next_page_id;

    void openFile();
};