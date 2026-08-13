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

    RID insert(const std::vector<std::string>& values);
    bool deleteByKey(const std::string& key);
    std::optional<Record> getByKey(const std::string& key);

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
