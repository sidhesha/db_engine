#pragma once

#include <fstream>
#include <string>
#include <unordered_map>
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
    struct TableFiles {
        std::string heap_filename;
        std::string index_filename;
    };

    // Single-table convenience constructor (table_id 0) -- every
    // pre-Phase-6 caller has exactly one table's worth of files and one
    // WAL, so this stays exactly as before.
    RecoveryManager(WALWriter& wal, const std::string& heap_filename, const std::string& index_filename);
    // Multi-table (Phase 6): one shared WAL, dispatched by each record's
    // table_id to that table's own heap/index files. `tables` must
    // contain an entry for every table_id that could appear in the WAL --
    // a record referencing an unknown table_id is a hard error (see
    // run()), not silently skipped, since a table that ever wrote to the
    // WAL always stays in the catalog (no DROP TABLE in this project).
    RecoveryManager(WALWriter& wal, std::unordered_map<uint32_t, TableFiles> tables);

    // Redo pass: replay every UPDATE/CLR record whose LSN is newer than
    // the target page's on-disk LSN (idempotent -- safe to redo
    // something already durable). Then undo pass: any txn_id with no
    // COMMIT record gets its UPDATEs reverted in reverse order via their
    // old_data images, each undo step itself logged as a CLR so a crash
    // during recovery is itself redoable.
    void run();

private:
    WALWriter& wal;
    std::unordered_map<uint32_t, TableFiles> tables;

    std::size_t heapOffset(int page_id) const;
    std::size_t indexOffset(int node_id) const;
    std::size_t lsnFieldOffset(WALStore store) const;

    void ensureOpen(std::fstream& file, const std::string& filename) const;
    uint64_t readStoredLSN(std::fstream& file, std::size_t page_offset, std::size_t lsn_offset) const;
    void writePageBytes(std::fstream& file, std::size_t page_offset, const std::vector<char>& data) const;
};
