#include "db_engine/lockmanager.hpp"
#include <algorithm>
#include <string>

void LockManager::acquireExclusive(const RID& rid, uint64_t txn_id, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu);

    while (true) {
        if (deadlock_victims.count(txn_id)) {
            deadlock_victims.erase(txn_id);
            throw DeadlockError("LockManager: txn " + std::to_string(txn_id) +
                                 " aborted to break a deadlock");
        }

        auto it = holder_of.find(rid);
        if (it == holder_of.end()) {
            holder_of[rid] = txn_id;
            held_by[txn_id].insert(rid);
            return;
        }
        if (it->second == txn_id) {
            return;  // already held by this transaction -- re-entrant
        }

        uint64_t holder = it->second;

        if (wouldCycleLocked(txn_id, holder)) {
            // A cycle exists: holder can already (transitively) reach
            // txn_id via waits_for, and txn_id is about to wait on
            // holder, closing the loop. Always victimize the
            // numerically-younger id so the outcome doesn't depend on
            // which thread's acquireExclusive call happens to notice
            // first -- both sides would compute the same max().
            uint64_t victim = std::max(txn_id, holder);
            if (victim == txn_id) {
                throw DeadlockError("LockManager: txn " + std::to_string(txn_id) +
                                     " aborted to break a deadlock with txn " + std::to_string(holder));
            }
            // The other side is the victim. It's asleep in its own
            // cv.wait_for below (that's what makes this a cycle), so it
            // can only discover this and throw on its own thread --
            // mark it and wake everyone to re-check.
            deadlock_victims.insert(victim);
            cv.notify_all();
            // Fall through to wait normally: once `victim` throws and is
            // released via releaseAll(), this lock (or the next one in
            // the chain) becomes available.
        }

        waits_for[txn_id] = holder;
        bool acquired_or_victim = cv.wait_for(lock, timeout, [&] {
            if (deadlock_victims.count(txn_id)) return true;
            auto cur = holder_of.find(rid);
            return cur == holder_of.end() || cur->second == txn_id;
        });
        waits_for.erase(txn_id);

        if (!acquired_or_victim) {
            throw LockTimeoutError("LockManager: txn " + std::to_string(txn_id) +
                                    " timed out waiting for a lock");
        }
        // Loop back: re-check deadlock_victims, or actually claim the
        // lock (another thread may have grabbed it first between wakeup
        // and reacquiring `lock`, so this can't just assume success).
    }
}

void LockManager::releaseAll(uint64_t txn_id) {
    std::lock_guard<std::mutex> lock(mu);
    auto it = held_by.find(txn_id);
    if (it != held_by.end()) {
        for (const RID& rid : it->second) {
            holder_of.erase(rid);
        }
        held_by.erase(it);
    }
    waits_for.erase(txn_id);
    deadlock_victims.erase(txn_id);
    cv.notify_all();
}

bool LockManager::wouldCycleLocked(uint64_t from_txn, uint64_t to_txn) const {
    uint64_t cur = to_txn;
    std::unordered_set<uint64_t> visited;
    while (true) {
        if (cur == from_txn) return true;
        if (!visited.insert(cur).second) return false;  // hit an unrelated cycle, not one through from_txn
        auto it = waits_for.find(cur);
        if (it == waits_for.end()) return false;
        cur = it->second;
    }
}
