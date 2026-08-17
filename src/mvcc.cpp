#include "db_engine/mvcc.hpp"
#include <stdexcept>
#include <string>

uint64_t MVCCManager::begin() {
    std::lock_guard<std::mutex> lock(mu);
    uint64_t id = txns.begin();

    Snapshot snap;
    snap.xmax = txns.peekNextTxnId();  // > id, since begin() above already advanced it
    for (const auto& [other_id, txn] : transactions) {
        if (txn.state == TxnState::ACTIVE) snap.active_at_start.insert(other_id);
    }

    transactions[id] = Transaction{id, TxnState::ACTIVE, snap};
    return id;
}

void MVCCManager::commit(uint64_t txn_id) {
    std::lock_guard<std::mutex> lock(mu);
    txns.commit(txn_id);
    // Erasing (rather than marking COMMITTED and keeping it around) is
    // safe *because* stateOfLocked treats "not found" as committed, and
    // it keeps this map from growing unboundedly over a long-running
    // process -- unlike an aborted txn's entry, nothing ever needs to
    // distinguish "committed" from "never tracked" again.
    transactions.erase(txn_id);
    lock_manager.releaseAll(txn_id);
}

void MVCCManager::abort(uint64_t txn_id) {
    std::lock_guard<std::mutex> lock(mu);
    txns.abort(txn_id);
    auto it = transactions.find(txn_id);
    if (it != transactions.end()) {
        // Unlike commit(), this entry must be kept (not erased): an
        // aborted txn_id has to keep reading back as ABORTED for the rest
        // of this process's lifetime, never falling through to
        // stateOfLocked's not-found-means-committed default.
        it->second.state = TxnState::ABORTED;
    }
    lock_manager.releaseAll(txn_id);
}

void MVCCManager::acquireExclusive(const RID& rid, uint64_t txn_id) {
    lock_manager.acquireExclusive(rid, txn_id);
}

Snapshot MVCCManager::getSnapshot(uint64_t txn_id) {
    std::lock_guard<std::mutex> lock(mu);
    if (txn_id == 0) {
        return snapshotLocked();
    }
    auto it = transactions.find(txn_id);
    if (it == transactions.end()) {
        throw std::runtime_error(
            "MVCCManager::getSnapshot: txn " + std::to_string(txn_id) +
            " is unknown or already committed/aborted");
    }
    return it->second.snapshot;
}

bool MVCCManager::isVisible(uint64_t create_txn_id, uint64_t delete_txn_id,
                             uint64_t reader_txn_id, const Snapshot& snapshot) const {
    std::lock_guard<std::mutex> lock(mu);

    bool self_created = (reader_txn_id != 0 && create_txn_id == reader_txn_id);
    if (!self_created) {
        TxnState create_state = stateOfLocked(create_txn_id);
        if (create_state != TxnState::COMMITTED) return false;  // aborted, or in-flight and not mine
        if (create_txn_id >= snapshot.xmax) return false;       // started after my snapshot
        if (snapshot.wasActiveAtSnapshot(create_txn_id)) return false;  // concurrent with my snapshot
    }
    // else: I created this version myself, in this same still-open
    // transaction -- always visible to me regardless of commit status.

    if (delete_txn_id == 0) return true;  // never deleted
    if (reader_txn_id != 0 && delete_txn_id == reader_txn_id) return false;  // I deleted it myself

    TxnState delete_state = stateOfLocked(delete_txn_id);
    if (delete_state != TxnState::COMMITTED) return true;  // not really deleted as far as I'm concerned yet
    if (delete_txn_id >= snapshot.xmax) return true;              // deleted after my snapshot -- I still see the old row
    if (snapshot.wasActiveAtSnapshot(delete_txn_id)) return true; // concurrent with my snapshot

    return false;  // deleted, committed, strictly before my snapshot
}

TxnState MVCCManager::stateOfLocked(uint64_t txn_id) const {
    auto it = transactions.find(txn_id);
    return it == transactions.end() ? TxnState::COMMITTED : it->second.state;
}

Snapshot MVCCManager::snapshotLocked() const {
    Snapshot snap;
    snap.xmax = txns.peekNextTxnId();
    for (const auto& [id, txn] : transactions) {
        if (txn.state == TxnState::ACTIVE) snap.active_at_start.insert(id);
    }
    return snap;
}
