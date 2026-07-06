#include "bufferpool.hpp"
#include <iostream>
#include <filesystem>
#include <cstring>
#include <stdexcept>

BufferPool::BufferPool(const std::string& fname)
    : filename(fname), next_page_id(0), frames(NUM_FRAMES), clock_hand(0) {
    openFile();

    std::filesystem::path path(filename);
    if (std::filesystem::exists(path)) {
        std::uintmax_t filesize = std::filesystem::file_size(path);
        next_page_id = filesize / PAGE_SIZE;
    }
}

BufferPool::~BufferPool() {
    flush();
    if (file.is_open()) file.close();
}

void BufferPool::openFile() {
    file.open(filename, std::ios::in | std::ios::out | std::ios::binary);

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

void BufferPool::ensureFileSize(std::size_t size) {
    file.seekp(0, std::ios::end);
    std::size_t current_size = file.tellp();
    if (current_size < size) {
        std::vector<char> zeros(size - current_size, 0);
        file.write(zeros.data(), zeros.size());
        file.flush();
    }
}

Page& BufferPool::fetchPage(int page_id) {
    int idx = findFrame(page_id);
    if (idx != -1) {
        frames[idx].pin_count++;
        frames[idx].ref_bit = true;
        return *frames[idx].page;
    }

    idx = evictFrame();
    readPageFromDisk(idx, page_id);
    frames[idx].pin_count = 1;
    frames[idx].ref_bit = true;
    return *frames[idx].page;
}

void BufferPool::unpinPage(int page_id, bool dirty) {
    int idx = findFrame(page_id);
    if (idx == -1) {
        throw std::runtime_error("unpinPage: page " + std::to_string(page_id) + " not in buffer pool");
    }
    if (frames[idx].pin_count > 0) {
        frames[idx].pin_count--;
    }
    if (dirty) {
        frames[idx].dirty = true;
    }
}

int BufferPool::allocatePage() {
    int page_id = next_page_id++;

    int idx = evictFrame();
    frames[idx].page = std::make_unique<Page>(page_id);
    frames[idx].page_id = page_id;
    frames[idx].dirty = true;
    frames[idx].pin_count = 0;
    frames[idx].ref_bit = false;

    return page_id;
}

int BufferPool::getNextPageId() const {
    return next_page_id;
}

void BufferPool::flush() {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (frames[i].page && frames[i].dirty) {
            writePageToDisk(i);
            frames[i].dirty = false;
        }
    }
    file.flush();
}

int BufferPool::findFrame(int page_id) const {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (frames[i].page && frames[i].page_id == page_id) {
            return i;
        }
    }
    return -1;
}

int BufferPool::evictFrame() {
    int attempts = 0;
    while (attempts < NUM_FRAMES * 2) {
        BufferFrame& frame = frames[clock_hand];

        if (frame.pin_count > 0) {
            clock_hand = (clock_hand + 1) % NUM_FRAMES;
            attempts++;
            continue;
        }

        if (frame.ref_bit) {
            frame.ref_bit = false;
            clock_hand = (clock_hand + 1) % NUM_FRAMES;
            attempts++;
            continue;
        }

        if (frame.page) {
            if (frame.dirty) {
                writePageToDisk(clock_hand);
                frame.dirty = false;
            }
            frame.page.reset();
            frame.page_id = -1;
        }

        int evicted_idx = clock_hand;
        clock_hand = (clock_hand + 1) % NUM_FRAMES;
        return evicted_idx;
    }

    for (int i = 0; i < NUM_FRAMES; i++) {
        BufferFrame& frame = frames[i];
        if (frame.pin_count == 0 && frame.page) {
            if (frame.dirty) {
                writePageToDisk(i);
                frame.dirty = false;
            }
            frame.page.reset();
            frame.page_id = -1;
            return i;
        }
    }

    throw std::runtime_error("BufferPool: all frames are pinned, cannot evict");
}

void BufferPool::readPageFromDisk(int frame_idx, int page_id) {
    std::vector<char> buffer(PAGE_SIZE);
    file.seekg(page_id * PAGE_SIZE, std::ios::beg);
    file.read(buffer.data(), PAGE_SIZE);
    if (!file) {
        throw std::runtime_error("Failed to read page " + std::to_string(page_id) + " from disk");
    }

    frames[frame_idx].page = std::make_unique<Page>(Page::deserialize(buffer));
    frames[frame_idx].page_id = page_id;
    frames[frame_idx].dirty = false;
}

void BufferPool::writePageToDisk(int frame_idx) {
    if (!frames[frame_idx].page) return;

    std::vector<char> buffer = frames[frame_idx].page->serialize();
    std::size_t offset = static_cast<std::size_t>(frames[frame_idx].page_id) * PAGE_SIZE;
    ensureFileSize(offset + PAGE_SIZE);

    file.seekp(offset, std::ios::beg);
    file.write(buffer.data(), PAGE_SIZE);
    if (!file) {
        throw std::runtime_error("Failed to write page " + std::to_string(frames[frame_idx].page_id) + " to disk");
    }
}
