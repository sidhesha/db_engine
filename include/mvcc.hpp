#pragma once

#include <mutex>
#include <unordered_map>
#include "lockmanager.hpp"
#include "transaction.hpp"
#include "wal.hpp"

// Owns in-memory transaction status + snapshot bookkeeping and the
// row-version visibility rule built on top of it. Layered on top of
// TransactionManager (which it wraps begin/commit/abort through, so every
// MVCC transaction is still a real, WAL-logged, recoverable one) rather
// than replacing it -- see transaction.hpp for why none of this needs to
// be durable itself.
class MVCCManager {
public:
    explicit MVCCManager(TransactionManager& txns) : txns(txns) {}

    uint64_t begin();
    void commit(uint64_t txn_id);
    void abort(uint64_t txn_id);

    // Acquires txn_id's exclusive write lock on `rid`, blocking until
    // available -- see LockManager for the blocking/deadlock semantics.
    // Released automatically by commit()/abort(); there's no separate
    // release call. Deliberately doesn't take MVCCManager's own `mu`
    // (LockManager has its own): blocking on a row lock while holding
    // `mu` would stall every other begin()/commit()/isVisible() call for
    // however long this wait takes, unlike everything else this class does.
    void acquireExclusive(const RID& rid, uint64_t txn_id);

    // The snapshot to evaluate visibility against for a given caller.
    // txn_id == 0 -- the same "no caller-owned transaction" sentinel used
    // everywhere else in this codebase -- takes a fresh snapshot of
    // right now (every prior committed write visible, nothing else),
    // since there's no multi-statement transaction whose original
    // begin()-time snapshot it should instead be pinned to.
    Snapshot getSnapshot(uint64_t txn_id);

    // Whether one row version (its create/delete txn ids, straight out of
    // Record's MVCC header) is visible to `reader_txn_id` under
    // `snapshot`. `reader_txn_id` is 0 for an ad hoc read with no
    // caller-owned transaction (see getSnapshot) -- never a real record's
    // create/delete txn id, since every write is stamped with a real,
    // already-begun id (see Table::insert), so it can never accidentally
    // satisfy the "read your own writes" self-checks below.
    bool isVisible(uint64_t create_txn_id, uint64_t delete_txn_id,
                    uint64_t reader_txn_id, const Snapshot& snapshot) const;

private:
    TransactionManager& txns;
    mutable std::mutex mu;
    // Only transactions begun by this MVCCManager instance during this
    // process's lifetime. A txn_id with no entry here is necessarily from
    // an earlier process and necessarily committed -- see stateOfLocked.
    std::unordered_map<uint64_t, Transaction> transactions;
    LockManager lock_manager;

    // Callers must already hold `mu`.
    TxnState stateOfLocked(uint64_t txn_id) const;
    Snapshot snapshotLocked() const;
};
