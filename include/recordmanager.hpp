#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "record.hpp"
#include "pagemanager.hpp"
#include "constants.hpp"

class RecordManager {
public:
    RecordManager(PageManager& page_manager);

    // txn_id == 0 (default): auto-commit, same as before Phase 5. A real
    // txn_id groups this write under a caller-owned transaction -- see
    // BufferPool::unpinPage/PageManager::writePage.
    RID insertRecord(const Record& record, uint64_t txn_id = 0);
    Record readRecord(const RID& rid);
    // Physical slot removal -- pre-MVCC behavior, kept for direct callers
    // that don't want version-chain semantics (e.g. lower-level tests).
    // Table (Phase 5) no longer calls this for a logical row delete --
    // see markDeleted.
    bool deleteRecord(const RID& rid, uint64_t txn_id = 0);

    // MVCC update (Phase 5): patches old_rid's delete_txn_id in place
    // (tombstoning that version -- readRecord/patchBytes both still work
    // on it, since is_active is untouched) and inserts a brand-new
    // version carrying new_fields, its create_txn_id stamped txn_id and
    // prev_version pointing back at old_rid. That backward chain of
    // full-tuple before-images *is* the undo log a rollback needs (see
    // record.hpp) -- recovery's ordinary redo/undo already makes both
    // writes atomic, same as any other txn-grouped pair of page writes.
    // Returns the new version's RID, or nullopt if old_rid's current
    // version is already deleted (nothing live to update).
    std::optional<RID> updateRecord(const RID& old_rid,
                                     const std::vector<std::string>& new_fields,
                                     uint64_t txn_id);

    // MVCC delete (Phase 5): patches the current version's delete_txn_id
    // in place. No new version, no physical removal -- the row's data
    // stays put so a reader with an older snapshot can still see it (see
    // Table::deleteByKey for why the index entry is deliberately left
    // pointing at it too). Returns false if already deleted.
    bool markDeleted(const RID& rid, uint64_t txn_id);

private:
    PageManager& page_manager;
    // Serializes every read-modify-write page access across concurrent
    // callers. Row-level locking (LockManager, Phase 5 Session 4) only
    // ever prevents two transactions from targeting the *same RID*
    // concurrently -- it says nothing about two different RIDs that
    // happen to physically live on the same page (routine: many small
    // rows get packed onto few pages), and nothing below RecordManager
    // (PageManager, BufferPool) has any synchronization of its own.
    // Without this, two threads racing a read-page/mutate-copy/write-page
    // sequence for different rows on the same page silently lose one
    // side's write -- confirmed the hard way via a genuinely lost delete
    // under the Session 5 stress test. Coarse (one mutex for the whole
    // heap, not per-page), the same tradeoff bplustree.cpp's io_mutex
    // already makes for the index side: correct is the priority here,
    // not maximum write throughput under contention, and I/O dominates
    // whatever brief serialization this adds. Doesn't reintroduce
    // "readers block on writers" in the MVCC sense -- this is
    // microsecond-scale physical I/O serialization, not waiting on
    // another transaction's commit/abort, exactly like the B+ tree's own
    // latches briefly serializing concurrent traversals.
    std::mutex io_mutex;

    // Shared by insertRecord() and updateRecord()'s new-version insert.
    // Caller must already hold io_mutex (std::mutex isn't recursive).
    RID insertRecordLocked(const Record& record, uint64_t txn_id);
};
