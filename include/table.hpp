#pragma once
#include "Schema.hpp"
#include "PageManager.hpp"
#include "BPlusTree.hpp"
#include "Record.hpp"
#include "recordmanager.hpp"
#include "indexmanager.hpp"

class Table {
public:
    // index_manager must outlive this Table -- it's what makes the
    // index actually durable/WAL-logged/recoverable, same as pm/rm.
    // Table used to default-construct an in-memory-only BPlusTree here,
    // which meant every index write silently bypassed all of Phases
    // 1-4's persistence/durability work; this wires it to the real one.
    Table(const std::string& name, const Schema& schema, PageManager& pm,
          RecordManager& rm, IndexManager& im);

    // txn_id == 0 (default): auto-commit, same as before Phase 5 -- each
    // call is its own independent transaction. A real, caller-supplied
    // txn_id (from MVCCManager::begin(), Session 3) groups this call's
    // row + index writes with other calls under one multi-statement
    // transaction; getByKey will use it for snapshot visibility once
    // that lands (Session 3) -- accepted now so callers don't need to
    // change again then.
    RID insert(const std::vector<std::string>& values, uint64_t txn_id = 0);
    bool deleteByKey(const std::string& key, uint64_t txn_id = 0);
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

};
