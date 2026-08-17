#include "db_engine/table.hpp"
#include <iostream>

namespace {
// RAII for the txn_id==0 auto-commit sentinel: aborts the transaction
// Table itself began if the guarded scope exits without an explicit
// commit() call -- an exception (e.g. RecordManager::insertRecord's
// "too large for a page"), or an early "nothing to do" return (key not
// found / already deleted). Without this, MVCCManager::begin()'s ACTIVE
// bookkeeping (and the WAL BEGIN record it wrote) would dangle forever
// instead of ever resolving to committed or aborted. A no-op when `owns`
// is false: a caller-supplied txn_id is entirely that caller's own
// responsibility, exactly like every other auto_commit pattern in this
// codebase (BufferPool::unpinPage, BPlusTree::saveDirty, ...).
class AutoCommitGuard {
public:
    AutoCommitGuard(MVCCManager& mvcc, uint64_t txn_id, bool owns)
        : mvcc(mvcc), txn_id(txn_id), owns(owns) {}
    ~AutoCommitGuard() {
        if (owns && !resolved) mvcc.abort(txn_id);
    }
    AutoCommitGuard(const AutoCommitGuard&) = delete;
    AutoCommitGuard& operator=(const AutoCommitGuard&) = delete;

    void commit() {
        if (owns) mvcc.commit(txn_id);
        resolved = true;
    }

private:
    MVCCManager& mvcc;
    uint64_t txn_id;
    bool owns;
    bool resolved = false;
};
}  // namespace

Table::Table(const std::string& name,
             const Schema& schema,
             PageManager& pm,
             RecordManager& rm,
             IndexManager& im,
             MVCCManager& mvcc)
    : name(name), schema(schema), record_manager(rm), page_manager(pm), index(im), mvcc(mvcc) {}

uint64_t Table::beginTxn() { return mvcc.begin(); }
void Table::commitTxn(uint64_t txn_id) { mvcc.commit(txn_id); }
void Table::abortTxn(uint64_t txn_id) { mvcc.abort(txn_id); }

RID Table::insert(const std::vector<std::string>& values, uint64_t txn_id) {
    if (values.empty()) {
        throw std::runtime_error("No values provided for insert.");
    }
    if (values.size() != schema.getColumns().size()) {
        throw std::runtime_error("Mismatched column count in insert.");
    }

    bool auto_commit = (txn_id == 0);
    uint64_t active = auto_commit ? mvcc.begin() : txn_id;
    AutoCommitGuard guard(mvcc, active, auto_commit);

    // Use first column as the primary key. A logical delete
    // (Table::deleteByKey) leaves its index entry in place, tombstoned
    // rather than removed, so re-inserting the same key after a
    // committed delete must repoint that existing entry rather than add
    // a second, duplicate one (the tree itself allows duplicate keys --
    // see the "duplicate key insert" BPlusTree test -- which would
    // otherwise leave two entries for one key with no defined winner for
    // a later search()). Chaining prev_version back to that tombstoned
    // entry, exactly like an update would, also keeps it reachable: a
    // reader whose snapshot predates the delete needs to be able to walk
    // past this new version to find the still-visible-to-it old one.
    const std::string& key = values[0];
    auto existing_rid = index.search(key);
    RID prev_version{-1, -1};
    if (existing_rid.has_value()) {
        // Blocks until no other transaction is concurrently writing this
        // same RID -- see LockManager. Acquired before reading `existing`
        // so that read can't observe a value another writer is
        // mid-mutation on.
        mvcc.acquireExclusive(existing_rid.value(), active);
        Record existing = record_manager.readRecord(existing_rid.value());
        if (existing.getDeleteTxnId() != 0) {
            prev_version = existing_rid.value();
        }
        // else: re-inserting a still-live key. Primary-key uniqueness
        // isn't enforced anywhere in this codebase (pre-existing, not a
        // Phase 5 concern) -- already undefined/unsupported usage, so
        // left unchained rather than inventing new semantics for it.
    }

    Record record(values, active, prev_version);
    RID rid = record_manager.insertRecord(record, active);
    if (existing_rid.has_value()) {
        // This new version becomes the current one for `key` the moment
        // index.update() below runs, well before this transaction
        // commits -- another transaction is free to index.search(key)
        // and land on `rid` from that point on. Lock it too (always
        // uncontended: `rid` was only just created, nothing else can
        // know about it yet) so such a transaction blocks on *this* one
        // instead of racing straight through unlocked.
        mvcc.acquireExclusive(rid, active);
        index.update(key, rid.page_id, rid.slot_id, active);
    } else {
        index.insert(key, rid.page_id, rid.slot_id, active);
    }

    guard.commit();
    return rid;
}

bool Table::updateByKey(const std::string& key, const std::vector<std::string>& values,
                         uint64_t txn_id) {
    if (values.size() != schema.getColumns().size()) {
        throw std::runtime_error("Mismatched column count in update.");
    }
    auto rid_opt = index.search(key);
    if (!rid_opt.has_value()) {
        return false;
    }

    bool auto_commit = (txn_id == 0);
    uint64_t active = auto_commit ? mvcc.begin() : txn_id;
    AutoCommitGuard guard(mvcc, active, auto_commit);

    // A transaction that's genuinely blocked here (someone else already
    // holds rid_opt's lock) always succeeds once woken: whoever released
    // it either committed (rid_opt is still the live, current version --
    // they only ever hold *their own new* version's lock, never this
    // one's replacement, until they've moved on) or aborted (rid_opt is
    // untouched). What isn't handled is a much narrower TOCTOU window:
    // rid_opt was read via index.search() *before* this lock, so a third,
    // faster transaction that races in and commits between that read and
    // this acquireExclusive call (never actually blocking this one --
    // nobody held the lock yet when it grabbed it) can still leave
    // rid_opt stale. updateRecord below reports that the same way it
    // would a genuine prior delete, rather than retrying against the new
    // head -- a production database's UPDATE would re-fetch and retry
    // (Postgres's EvalPlanQual), which is a meaningfully bigger feature
    // this project doesn't take on.
    mvcc.acquireExclusive(rid_opt.value(), active);

    auto new_rid = record_manager.updateRecord(rid_opt.value(), values, active);
    if (!new_rid.has_value()) {
        return false;  // already deleted -- guard aborts the no-op transaction
    }
    mvcc.acquireExclusive(*new_rid, active);  // see the matching comment in insert()
    index.update(key, new_rid->page_id, new_rid->slot_id, active);

    guard.commit();
    return true;
}

std::optional<Record> Table::findVisibleVersion(RID start_rid, uint64_t txn_id, const Snapshot& snapshot) {
    RID current_rid = start_rid;
    while (true) {
        Record rec = record_manager.readRecord(current_rid);
        if (mvcc.isVisible(rec.getCreateTxnId(), rec.getDeleteTxnId(), txn_id, snapshot)) {
            return rec;
        }
        if (!rec.hasPrevVersion()) {
            return std::nullopt;
        }
        current_rid = rec.getPrevVersion();
    }
}

std::optional<Record> Table::getByKey(const std::string& key, uint64_t txn_id) {
    auto rid_opt = index.search(key);
    if (!rid_opt.has_value()) {
        return std::nullopt;
    }

    Snapshot snapshot = mvcc.getSnapshot(txn_id);
    return findVisibleVersion(rid_opt.value(), txn_id, snapshot);
}

std::vector<std::pair<Key, Record>> Table::scanAll(uint64_t txn_id) {
    Snapshot snapshot = mvcc.getSnapshot(txn_id);
    std::vector<std::pair<Key, Record>> results;
    for (const auto& [key, rid] : index.getAllKeyRIDPairs()) {
        auto visible = findVisibleVersion(rid, txn_id, snapshot);
        if (visible.has_value()) {
            results.emplace_back(key, std::move(visible.value()));
        }
    }
    return results;
}


bool Table::deleteByKey(const std::string& key, uint64_t txn_id) {
    auto rid_opt = index.search(key);
    if (!rid_opt.has_value()) {
        return false;
    }

    bool auto_commit = (txn_id == 0);
    uint64_t active = auto_commit ? mvcc.begin() : txn_id;
    AutoCommitGuard guard(mvcc, active, auto_commit);

    mvcc.acquireExclusive(rid_opt.value(), active);  // see updateByKey's comment on staleness

    bool deleted = record_manager.markDeleted(rid_opt.value(), active);
    if (deleted) guard.commit();
    return deleted;
}

const std::string& Table::getName() const{
    return name;
}

const Schema& Table::getSchema() const{
    return schema;
}

void Table::printAll() {
    auto key_rid_pairs = index.getAllKeyRIDPairs();

    for (const auto& [key, rid] : key_rid_pairs) {
        auto rec = record_manager.readRecord(rid);
        const auto& fields = rec.getFields();
        for (const auto& val : fields) {
            std::cout << val << " ";
        }
        std::cout << "\n";
    }
}
