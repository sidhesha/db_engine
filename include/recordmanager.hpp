#pragma once

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
};
