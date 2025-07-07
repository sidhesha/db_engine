#pragma once
#include "Schema.hpp"
#include "PageManager.hpp"
#include "BPlusTree.hpp"
#include "Record.hpp"
#include "recordmanager.hpp"

class Table {
public:
    Table(const std::string& name, const Schema& schema, PageManager& pm, RecordManager& rm);

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
