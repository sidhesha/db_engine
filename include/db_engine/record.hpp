#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "db_engine/constants.hpp"

// A Record is one version of a logical row. MVCC (Phase 5) never
// overwrites a version's fields in place: an UPDATE stamps the OLD
// version's delete_txn_id and inserts a brand-new version whose
// prev_version points back at it, forming a backward chain of full-tuple
// before-images per row -- that chain IS the undo log a rollback needs,
// stored as ordinary heap tuples instead of a separate structure (see
// CONCURRENCY_BUGS.md-adjacent design notes / the Phase 5 plan).
//
// create_txn_id == 0 and delete_txn_id == 0 are the "untracked" defaults
// -- consistent with TransactionManager's txn_id==0 sentinel meaning "no
// caller-owned transaction" elsewhere in this codebase, so records
// written by non-transactional (legacy) callers round-trip unchanged.
// prev_version == RID{-1, -1} means this is the first version of its row.
class Record {
public:
    Record() = default;
    explicit Record(const std::vector<std::string>& fields);
    Record(const std::vector<std::string>& fields, uint64_t create_txn_id,
           RID prev_version = RID{-1, -1});

    std::vector<std::string> getFields() const;

    uint64_t getCreateTxnId() const { return create_txn_id_; }
    uint64_t getDeleteTxnId() const { return delete_txn_id_; }
    void setDeleteTxnId(uint64_t txn_id) { delete_txn_id_ = txn_id; }
    RID getPrevVersion() const { return prev_version_; }
    bool hasPrevVersion() const { return prev_version_.page_id >= 0; }

    std::vector<char> serialize() const;
    static Record deserialize(const std::vector<char>& buffer);

    size_t size() const;

    // Fixed byte offset of delete_txn_id within serialize()'s output --
    // the same for every record regardless of field count, so
    // Page::patchBytes() can stamp it on an existing on-disk version
    // (marking it superseded/deleted) without touching anything else.
    static constexpr size_t DELETE_TXN_ID_OFFSET = sizeof(uint64_t);
    static constexpr size_t MVCC_HEADER_SIZE =
        sizeof(uint64_t) * 2 + sizeof(int32_t) * 2;  // create+delete txn id, prev_version{page,slot}

private:
    std::vector<std::string> fields_;
    uint64_t create_txn_id_ = 0;
    uint64_t delete_txn_id_ = 0;
    RID prev_version_ = RID{-1, -1};
};
