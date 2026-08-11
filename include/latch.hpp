#pragma once
#include <atomic>
#include <thread>

// Reader-writer spinlock used to latch individual B+ tree nodes.
// Deliberately not std::mutex/std::shared_mutex: a real latch-crabbing
// implementation holds many short-lived latches per operation, so it
// needs to be cheap to acquire/release and to support upgrade-free
// shared (S) and exclusive (X) modes without OS-level blocking.
//
// state == 0   -> free
// state == -1  -> held exclusively
// state > 0    -> held by that many shared (reader) holders
//
// Writer priority: lockShared() defers (without touching `state`)
// whenever a writer is waiting, via `waiting_writers`. Without this, a
// latch under a continuous stream of shared acquirers -- e.g.
// structure_latch, taken shared by every insert()'s descent -- can starve
// an exclusive lock() indefinitely: each individual reader's hold is
// short, but if new ones keep arriving before `state` ever reaches 0, the
// writer's CAS never gets a window. This isn't hypothetical: confirmed by
// hanging real 8-thread concurrent-insert runs under CPU-constrained
// conditions (2 cores) via gdb thread dump, all spinning on this exact
// lock with no forward progress -- the same pathology pthread_rwlock's
// PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP mode exists to prevent.
class RWSpinLatch {
public:
    RWSpinLatch() = default;

    // Copying/moving a latch never copies lock state -- the destination
    // simply gets its own fresh, unlocked latch. This lets BPlusTreeNode
    // (which embeds a latch) keep normal value semantics (it's copied/
    // returned by value in a few places, e.g. deserialize()).
    RWSpinLatch(const RWSpinLatch&) noexcept {}
    RWSpinLatch(RWSpinLatch&&) noexcept {}
    RWSpinLatch& operator=(const RWSpinLatch&) noexcept { return *this; }
    RWSpinLatch& operator=(RWSpinLatch&&) noexcept { return *this; }

    void lockShared() {
        for (;;) {
            if (waiting_writers.load(std::memory_order_relaxed) > 0) {
                // A writer is queued: don't join the reader side (that's
                // exactly the pattern that starves it). Existing readers
                // still drain normally via unlockShared(), so `state`
                // keeps moving toward 0 instead of being kept aloft by a
                // constant stream of new arrivals.
                std::this_thread::yield();
                continue;
            }
            int expected = state.load(std::memory_order_relaxed);
            if (expected >= 0 &&
                state.compare_exchange_weak(expected, expected + 1,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
                return;
            }
            std::this_thread::yield();
        }
    }

    void unlockShared() {
        state.fetch_sub(1, std::memory_order_release);
    }

    void lock() {
        waiting_writers.fetch_add(1, std::memory_order_relaxed);
        for (;;) {
            int expected = 0;
            if (state.compare_exchange_weak(expected, -1,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
                waiting_writers.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
            std::this_thread::yield();
        }
    }

    // Non-blocking: returns immediately (true/false) instead of
    // spinning. Needed for latch acquisitions that go against the
    // usual left-to-right, parent-to-child ordering every traversal
    // (moveRight, descent) follows -- e.g. remove()'s rebalancing
    // grabbing a *left* sibling while already holding the current node.
    // Unconditionally blocking there is a classic AB-BA deadlock
    // against a concurrent reader/writer approaching from the left via
    // moveRight. The standard fix (real lock managers do this) is a
    // conditional request: try, and back off/retry the whole operation
    // if it's not immediately available, rather than blocking and
    // risking a cycle.
    bool tryLock() {
        int expected = 0;
        return state.compare_exchange_strong(expected, -1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed);
    }

    void unlock() {
        state.store(0, std::memory_order_release);
    }

private:
    std::atomic<int> state{0};
    std::atomic<int> waiting_writers{0};
};

// RAII guards for a standalone RWSpinLatch (as opposed to LatchHandle,
// which specifically wraps a BPlusTreeNode's embedded latch).
class SharedLatchGuard {
public:
    explicit SharedLatchGuard(RWSpinLatch& l) : latch_(&l) { latch_->lockShared(); }
    ~SharedLatchGuard() { if (latch_) latch_->unlockShared(); }
    SharedLatchGuard(const SharedLatchGuard&) = delete;
    SharedLatchGuard& operator=(const SharedLatchGuard&) = delete;
    SharedLatchGuard(SharedLatchGuard&& other) noexcept : latch_(other.latch_) { other.latch_ = nullptr; }
    void release() { if (latch_) { latch_->unlockShared(); latch_ = nullptr; } }
private:
    RWSpinLatch* latch_;
};

class ExclusiveLatchGuard {
public:
    explicit ExclusiveLatchGuard(RWSpinLatch& l) : latch_(&l) { latch_->lock(); }
    ~ExclusiveLatchGuard() { if (latch_) latch_->unlock(); }
    ExclusiveLatchGuard(const ExclusiveLatchGuard&) = delete;
    ExclusiveLatchGuard& operator=(const ExclusiveLatchGuard&) = delete;
    ExclusiveLatchGuard(ExclusiveLatchGuard&& other) noexcept : latch_(other.latch_) { other.latch_ = nullptr; }
    void release() { if (latch_) { latch_->unlock(); latch_ = nullptr; } }
private:
    RWSpinLatch* latch_;
};
