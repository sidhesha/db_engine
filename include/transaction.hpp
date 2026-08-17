#pragma once

#include <cstdint>
#include <unordered_set>

// In-memory-only transaction bookkeeping for MVCC visibility (Phase 5).
// Distinct from TransactionManager (wal.hpp), which owns durable
// BEGIN/COMMIT/ABORT WAL records and txn_id allocation -- this layer
// tracks *status* (so a reader can tell committed from aborted from
// still-in-flight) and hands out snapshots, neither of which needs to
// survive a restart: RecoveryManager::run() already guarantees only
// committed data is on disk by the time anything reads this, so a fresh
// process starting with empty bookkeeping is correct by construction.
enum class TxnState { ACTIVE, COMMITTED, ABORTED };

// A fixed point in transaction-id order that a reader's visibility check
// is evaluated against, capturing "the state of the world as of when this
// snapshot was taken" (REPEATABLE READ / snapshot isolation -- it does not
// move as other transactions commit around it).
struct Snapshot {
    // Any txn_id >= xmax was allocated after this snapshot was taken, so
    // its effects (committed or not) are never visible to it.
    uint64_t xmax = 0;
    // txn_ids that were still ACTIVE (begun, not yet committed/aborted) at
    // the moment this snapshot was taken. A member of this set that later
    // commits is still invisible: it was concurrent with this snapshot,
    // not before it. Mirrors Postgres's xip_list.
    std::unordered_set<uint64_t> active_at_start;

    bool wasActiveAtSnapshot(uint64_t txn_id) const {
        return active_at_start.count(txn_id) != 0;
    }
};

struct Transaction {
    uint64_t id = 0;
    TxnState state = TxnState::ACTIVE;
    Snapshot snapshot;
};
