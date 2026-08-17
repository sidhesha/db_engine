#pragma once

#include "Schema.hpp"
#include <cstdint>
#include <unordered_map>
#include <string>
#include <fstream>

// One table's catalog entry: its schema plus the id (Phase 6) that
// deterministically derives its physical heap/index filenames and tags
// its WAL records so a shared, multi-table WAL can tell them apart --
// see Database and RecoveryManager.
struct TableMeta {
    uint32_t table_id;
    Schema schema;
};

class CatalogManager {
public:
    CatalogManager(const std::string& catalog_file);

    // Assigns and returns a fresh table_id, persisted immediately.
    uint32_t createTable(const std::string& table_name, const Schema& schema);
    Schema getSchema(const std::string& table_name) const;
    uint32_t getTableId(const std::string& table_name) const;
    bool hasTable(const std::string& table_name) const;
    const std::unordered_map<std::string, TableMeta>& getAllTables() const;

    void loadCatalog();
    void saveCatalog();

private:
    std::string catalog_file;
    std::unordered_map<std::string, TableMeta> tables;
    // Next id to assign. Recomputed from the loaded catalog on
    // construction (max existing table_id + 1), the same "derive from
    // what's already on disk rather than persist a separate counter"
    // approach TransactionManager already uses for next_txn_id.
    uint32_t next_table_id = 0;
};