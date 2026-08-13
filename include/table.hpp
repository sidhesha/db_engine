#pragma once
#include "Schema.hpp"
#include "PageManager.hpp"
#include "BPlusTree.hpp"
#include "Record.hpp"
#include "recordmanager.hpp"
#include "indexmanager.hpp"
#include "mvcc.hpp"

class Table {
public:
    // index_manager must outlive this Table -- it's what makes the
    // index actually durable/WAL-logged/recoverable, same as pm/rm.
    // Table used to default-construct an in-memory-only BPlusTree here,
    // which meant every index write silently bypassed all of Phases
    // 1-4's persistence/durability work; this wires it to the real one.
    Table(const std::string& name, const Schema& schema, PageManager& pm,
          RecordManager& rm, IndexManager& im);

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

    const std::string& getName() const;
    const Schema& getSchema() const;
    void printAll();
private:
    std::string name;
    Schema schema;
    RecordManager record_manager;
    PageManager& page_manager;
    BPlusTree index;
    MVCCManager mvcc;
};
