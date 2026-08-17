#pragma once

#include "page.hpp"
#include "wal.hpp"
#include "evictionpolicy.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <memory>

struct BufferFrame {
    std::unique_ptr<Page> page;
    int page_id;
    bool dirty;
    int pin_count;

    // Snapshot of this page's bytes as of the most recent fetchPage()
    // since it was last logged/clean -- the WAL "before image" for
    // whatever mutation the caller makes through the reference returned
    // by fetchPage() before calling unpinPage(id, true). Captured
    // eagerly at fetch time (rather than lazily on first write) because
    // BufferPool's contract hands callers a mutable Page& directly, with
    // no separate "I'm about to write" call to hook into.
    std::vector<char> before_image;
    bool before_image_valid;

    BufferFrame() : page(nullptr), page_id(-1), dirty(false), pin_count(0), before_image_valid(false) {}
};

class BufferPool {
public:
    static constexpr int DEFAULT_NUM_FRAMES = 64;

    // wal/txns must outlive this BufferPool. They're taken by reference
    // (not owned) because a correct WAL spans every store in the engine --
    // IndexManager logs through the same WALWriter/TransactionManager so
    // there's one LSN space and one txn_id space across heap and index.
    // table_id defaults to 0 (source-compatible with every pre-Phase-6
    // single-table caller); Database (Phase 6) passes a real, distinct id
    // per table so its shared WAL/TransactionManager can tell which
    // table's heap file a given UPDATE record belongs to.
    // policy defaults to ClockSweepPolicy (source-compatible with every
    // pre-Phase-7 caller) -- Phase 7's eviction-policy shootout
    // constructs a BufferPool with LRU2Policy instead to compare the two
    // head to head.
    // num_frames defaults to DEFAULT_NUM_FRAMES (64, source-compatible
    // with every pre-Phase-7 caller) -- Phase 7's buffer-pool-on-vs-off
    // benchmark runs the same, real BufferPool at num_frames=1 (evicts on
    // nearly every access, functionally "no cache") against the default,
    // rather than maintaining a second, parallel no-cache implementation.
    BufferPool(const std::string& filename, WALWriter& wal, TransactionManager& txns,
               uint32_t table_id = 0,
               std::unique_ptr<EvictionPolicy> policy = std::make_unique<ClockSweepPolicy>(),
               int num_frames = DEFAULT_NUM_FRAMES);
    ~BufferPool();

    Page& fetchPage(int page_id);
    // txn_id == 0 (the default) means "no caller-owned transaction":
    // logUpdateIfChanged() auto-begins/commits its own transaction, same
    // as before Phase 5. A real, caller-supplied txn_id defers commit
    // to the caller, so this page write can be grouped atomically with
    // other writes (heap or index) under one multi-statement transaction.
    void unpinPage(int page_id, bool dirty, uint64_t txn_id = 0);
    int allocatePage();
    int getNextPageId() const;
    void flush();

private:
    std::string filename;
    std::fstream file;
    int next_page_id;

    WALWriter& wal;
    TransactionManager& txns;
    uint32_t table_id;

    std::vector<BufferFrame> frames;
    std::unique_ptr<EvictionPolicy> policy;

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
    // changed since captureBeforeImage(), logs an UPDATE record (WAL
    // rule: flushed durably before this page can reach disk) and stamps
    // the page's LSN. Auto-begins/commits its own transaction when
    // txn_id == 0; otherwise appends under the caller's transaction and
    // leaves commit/abort to them.
    void logUpdateIfChanged(int idx, int page_id, uint64_t txn_id);
};
