#pragma once
#include "db_engine/schema.hpp"
#include "db_engine/pagemanager.hpp"
#include "db_engine/bplustree.hpp"
#include "db_engine/record.hpp"
#include "db_engine/recordmanager.hpp"
#include "db_engine/indexmanager.hpp"
#include "db_engine/mvcc.hpp"
#include "db_engine/key.hpp"

class Table {
public:
    // index_manager must outlive this Table -- it's what makes the
    // index actually durable/WAL-logged/recoverable, same as pm/rm.
    // Table used to default-construct an in-memory-only BPlusTree here,
    // which meant every index write silently bypassed all of Phases
    // 1-4's persistence/durability work; this wires it to the real one.
    //
    // mvcc is a reference, not an owned member (Phase 6): once multiple
    // tables can share one TransactionManager/WAL (Database), they must
    // also share one MVCCManager -- its in-memory transaction status and
    // snapshot bookkeeping (and the LockManager nested inside it) has to
    // be visible to every table a connection's transaction touches, not
    // duplicated per table. Before Phase 6, each Table had its own
    // isolated TransactionManager anyway, so this was equivalent to
    // owning one; now the caller (a Database, or a test wiring up a
    // single table exactly as before) owns it instead.
    Table(const std::string& name, const Schema& schema, PageManager& pm,
          RecordManager& rm, IndexManager& im, MVCCManager& mvcc);

    // Multi-statement transactions: begin, do several insert/updateByKey/
    // deleteByKey/getByKey calls passing the returned id as txn_id, then
    // commit or abort. Thin delegation to this Table's own MVCCManager --
    // there's deliberately no way to reach the underlying
    // TransactionManager directly, since MVCCManager's in-memory status/
    // snapshot bookkeeping has to stay in sync with every begin/commit/
    // abort or visibility silently breaks.
    uint64_t beginTxn();
    void commitTxn(uint64_t txn_id);
    void abortTxn(uint64_t txn_id);

    // txn_id == 0 (default): auto-commit, same as before Phase 5 -- each
    // call resolves, uses, and resolves its own real transaction under
    // the hood (see table.cpp's AutoCommitGuard) so a single statement's
    // heap + index writes are still one atomic unit. A real,
    // caller-supplied txn_id (from beginTxn()) groups this call with
    // others under one multi-statement transaction instead.
    RID insert(const std::vector<std::string>& values, uint64_t txn_id = 0);
    // MVCC update: the old version is tombstoned (delete_txn_id stamped),
    // a new version is inserted and chained back to it, and the index is
    // repointed at the new version -- see RecordManager::updateRecord.
    // Returns false if the key doesn't exist or is already deleted.
    bool updateByKey(const std::string& key, const std::vector<std::string>& values,
                      uint64_t txn_id = 0);
    // MVCC delete: logical tombstone only (RecordManager::markDeleted) --
    // deliberately does NOT remove the index entry, so a reader with an
    // older snapshot can still walk the version chain from it. Returns
    // false if the key doesn't exist or is already deleted.
    bool deleteByKey(const std::string& key, uint64_t txn_id = 0);
    // Walks the version chain (Record::prev_version) starting from
    // whatever the index currently points at until it finds a version
    // visible to txn_id's snapshot (see MVCCManager::isVisible), or falls
    // off the end of the chain.
    std::optional<Record> getByKey(const std::string& key, uint64_t txn_id = 0);
    // Full-table scan (Phase 6: SQL SELECT/UPDATE/DELETE with a WHERE
    // condition on a non-primary-key column has no index to use, so it
    // needs every visible row). One entry per key currently in the
    // index, using the exact same version-chain walk getByKey does (see
    // findVisibleVersion) -- a key whose current version isn't visible to
    // txn_id's snapshot is simply omitted, not returned as a "deleted"
    // marker.
    std::vector<std::pair<Key, Record>> scanAll(uint64_t txn_id = 0);

    const std::string& getName() const;
    const Schema& getSchema() const;
    void printAll();
private:
    std::string name;
    Schema schema;
    // Reference, not a copy: RecordManager now owns real per-instance
    // state (its io_mutex, serializing concurrent page access -- see
    // recordmanager.hpp), so every caller sharing one Table needs to be
    // going through the *same* RecordManager the constructor was given,
    // not a disconnected copy of it. Matches page_manager below, which
    // was already a reference for the same reason.
    RecordManager& record_manager;
    PageManager& page_manager;
    BPlusTree index;
    // Reference (see the constructor comment): shared across every Table
    // in the same Database so transaction/snapshot state and row locks
    // are visible engine-wide, not just within one table.
    MVCCManager& mvcc;

    // Shared by getByKey() and scanAll(): walks the version chain
    // starting at start_rid until it finds a version isVisible() to
    // txn_id under `snapshot`, or falls off the end.
    std::optional<Record> findVisibleVersion(RID start_rid, uint64_t txn_id, const Snapshot& snapshot);
};
