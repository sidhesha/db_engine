#include "table.hpp"
#include <iostream>

Table::Table(const std::string& name,
             const Schema& schema,
             PageManager& pm,
             RecordManager& rm)
    : name(name), schema(schema), page_manager(pm), record_manager(rm), index() {}


RID Table::insert(const std::vector<std::string>& values) {
    if (values.size() != schema.getColumns().size()) {
        throw std::runtime_error("Mismatched column count in insert.");
    }

    // Create a Record object from values
    Record record(values);

    // Delegate to RecordManager
    RID rid = record_manager.insertRecord(record);

    // Use first column as the primary key
    std::string key = values[0];
    index.insert(key, rid.page_id, rid.slot_id);

    return rid;
}

std::optional<Record> Table::getByKey(const std::string& key) {
    auto rid_opt = index.search(key);
    if (!rid_opt.has_value()) {
        return std::nullopt;
    }
    return record_manager.readRecord(rid_opt.value());
}


bool Table::deleteByKey(const std::string& key) {
    auto rid_opt = index.search(key);
    if (!rid_opt.has_value()) {
        return false;
    }

    bool deleted = record_manager.deleteRecord(rid_opt.value());
    if (deleted) {
        index.remove(key);  // not implemented yet
    }
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