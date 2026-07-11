#pragma once

#include "page.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <memory>

struct BufferFrame {
    std::unique_ptr<Page> page;
    int page_id;
    bool dirty;
    int pin_count;
    bool ref_bit;

    BufferFrame() : page(nullptr), page_id(-1), dirty(false), pin_count(0), ref_bit(false) {}
};

class BufferPool {
public:
    static constexpr int NUM_FRAMES = 64;

    explicit BufferPool(const std::string& filename);
    ~BufferPool();

    Page& fetchPage(int page_id);
    void unpinPage(int page_id, bool dirty);
    int allocatePage();
    int getNextPageId() const;
    void flush();

private:
    std::string filename;
    std::fstream file;
    int next_page_id;

    std::vector<BufferFrame> frames;
    int clock_hand;

    void openFile();
    void ensureFileSize(std::size_t size);
    int evictFrame();
    void readPageFromDisk(int frame_idx, int page_id);
    void writePageToDisk(int frame_idx);
    int findFrame(int page_id) const;
};
