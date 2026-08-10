#pragma once

#include <fstream>
#include <string>
#include "wal.hpp"

// ARIES-style crash recovery, run once at engine startup before
// BufferPool/IndexManager open their files for normal use. Operates
// directly on the raw heap/index files (not through BufferPool or
// IndexManager) since those classes' own constructors assume a
// consistent starting state -- recovery is what establishes that state.
//
// No checkpointing yet (acceptable at this project's scale): every run()
// call does a full scan of the WAL from the start. A future optimization,
// not a correctness requirement -- the redo/undo logic below is correct
// regardless of log length, just not the fastest possible.
class RecoveryManager {
public:
    RecoveryManager(WALWriter& wal, const std::string& heap_filename, const std::string& index_filename);

    // Redo pass: replay every UPDATE/CLR record whose LSN is newer than
    // the target page's on-disk LSN (idempotent -- safe to redo
    // something already durable). Then undo pass: any txn_id with no
    // COMMIT record gets its UPDATEs reverted in reverse order via their
    // old_data images, each undo step itself logged as a CLR so a crash
    // during recovery is itself redoable.
    void run();

private:
    WALWriter& wal;
    std::string heap_filename;
    std::string index_filename;

    std::size_t heapOffset(int page_id) const;
    std::size_t indexOffset(int node_id) const;
    std::size_t lsnFieldOffset(WALStore store) const;

    void ensureOpen(std::fstream& file, const std::string& filename) const;
    uint64_t readStoredLSN(std::fstream& file, std::size_t page_offset, std::size_t lsn_offset) const;
    void writePageBytes(std::fstream& file, std::size_t page_offset, const std::vector<char>& data) const;
};
