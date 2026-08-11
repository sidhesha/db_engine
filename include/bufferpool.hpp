#pragma once

#include "page.hpp"
#include "wal.hpp"
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

    // Snapshot of this page's bytes as of the most recent fetchPage()
    // since it was last logged/clean -- the WAL "before image" for
    // whatever mutation the caller makes through the reference returned
    // by fetchPage() before calling unpinPage(id, true). Captured
    // eagerly at fetch time (rather than lazily on first write) because
    // BufferPool's contract hands callers a mutable Page& directly, with
    // no separate "I'm about to write" call to hook into.
    std::vector<char> before_image;
    bool before_image_valid;

    BufferFrame() : page(nullptr), page_id(-1), dirty(false), pin_count(0), ref_bit(false), before_image_valid(false) {}
};

class BufferPool {
public:
    static constexpr int NUM_FRAMES = 64;

    // wal/txns must outlive this BufferPool. They're taken by reference
    // (not owned) because a correct WAL spans every store in the engine --
    // IndexManager logs through the same WALWriter/TransactionManager so
    // there's one LSN space and one txn_id space across heap and index.
    BufferPool(const std::string& filename, WALWriter& wal, TransactionManager& txns);
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

    WALWriter& wal;
    TransactionManager& txns;

    std::vector<BufferFrame> frames;
    int clock_hand;

    void openFile();
    void ensureFileSize(std::size_t size);
    int evictFrame();
    void readPageFromDisk(int frame_idx, int page_id);
    void writePageToDisk(int frame_idx);
    int findFrame(int page_id) const;

    // Snapshots frames[idx].page's current bytes as the WAL before-image,
    // if one isn't already pending for this dirty cycle.
    void captureBeforeImage(int idx);
    // Called from unpinPage(id, true): if the page's bytes actually
    // changed since captureBeforeImage(), logs one auto-commit UPDATE
    // record (WAL rule: flushed durably before this page can reach disk)
    // and stamps the page's LSN.
    void logUpdateIfChanged(int idx, int page_id);
};
