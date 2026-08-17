#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include "constants.hpp"

// Row-level (RID-keyed) write-write locking for MVCC (Phase 5). MVCC's
// whole point is readers never block: there are no read locks here, only
// concurrent writers targeting the same row version need to serialize --
// see Table::updateByKey/deleteByKey/insert for the call sites.
//
// Real OS-level blocking (std::mutex + std::condition_variable),
// deliberately NOT RWSpinLatch (latch.hpp): that primitive is a busy-spin
// (yield-loop) design purpose-built for holding many microsecond-scale
// latches per B-tree operation, explicitly documented there as needing
// to avoid OS-level blocking. A row lock is the opposite case -- it can
// be held for an entire multi-statement transaction, an arbitrarily long
// wait as far as this class is concerned -- so spinning a CPU core the
// whole time would be actively harmful rather than just non-optimal.
//
// Deadlock detection is a real waits-for graph (txn_id -> txn_id it's
// currently blocked on), not timeout-only: every transaction has at most
// one outstanding wait at a time (one statement executes at a time in
// this codebase), so the graph is a functional graph (out-degree <= 1)
// and "does adding this edge close a cycle" is a plain chain walk, not a
// general graph search. The timeout is a fallback bound for a wait that
// never resolves any other way, not the primary detection mechanism.
class LockManager {
public:
    // Thrown by acquireExclusive on the transaction chosen as the
    // cycle-breaking victim of a detected deadlock: always the
    // numerically-younger (higher, i.e. more-recently-begun) of the two
    // txn_ids forming the cycle, so the choice is the same no matter
    // which side's thread happens to detect it first.
    class DeadlockError : public std::runtime_error {
    public:
        explicit DeadlockError(const std::string& msg) : std::runtime_error(msg) {}
    };
    // Thrown when a wait exceeds `timeout` without a cycle ever being
    // detected (e.g. the holder is just slow, not deadlocked).
    class LockTimeoutError : public std::runtime_error {
    public:
        explicit LockTimeoutError(const std::string& msg) : std::runtime_error(msg) {}
    };

    // Blocks until `txn_id` holds the exclusive lock on `rid`. A txn_id
    // that already holds it (e.g. two writes to the same row in one
    // transaction) returns immediately. Throws DeadlockError or
    // LockTimeoutError instead of returning if this call can't ever
    // (cleanly) succeed -- see the class-level comment.
    void acquireExclusive(const RID& rid, uint64_t txn_id,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    // Releases every lock txn_id holds. Called exactly once per
    // transaction, on commit or abort (see MVCCManager::commit/abort) --
    // never needs an explicit per-lock release. Wakes anyone waiting on
    // any of them.
    void releaseAll(uint64_t txn_id);

private:
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<RID, uint64_t> holder_of;                    // rid -> holding txn_id
    std::unordered_map<uint64_t, std::unordered_set<RID>> held_by;  // txn -> rids it holds
    std::unordered_map<uint64_t, uint64_t> waits_for;                // txn -> txn it's blocked on, if any
    // Transactions woken specifically to throw DeadlockError, set by
    // whichever thread detects the cycle (which may be neither side of
    // the eventual victim's own wait -- see acquireExclusive).
    std::unordered_set<uint64_t> deadlock_victims;

    // Would adding a wait edge from_txn -> to_txn close a cycle back to
    // from_txn (i.e. can to_txn already reach from_txn via existing
    // waits_for edges)? Caller must already hold `mu`.
    bool wouldCycleLocked(uint64_t from_txn, uint64_t to_txn) const;
};
