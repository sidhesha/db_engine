#include "CatalogManager.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>

CatalogManager::CatalogManager(const std::string& catalog_file)
    : catalog_file(catalog_file) {
    loadCatalog(); //to in memory
}

uint32_t CatalogManager::createTable(const std::string& table_name, const Schema& schema) {
    if (hasTable(table_name)) {
        throw std::runtime_error("Table already exists: " + table_name);
    }

    uint32_t id = next_table_id++;
    tables[table_name] = TableMeta{id, schema};
    saveCatalog();
    return id;
}

Schema CatalogManager::getSchema(const std::string& table_name) const {
    auto it = tables.find(table_name);
    if (it == tables.end()) {
        throw std::runtime_error("Table not found: " + table_name);
    }
    return it->second.schema;
}

uint32_t CatalogManager::getTableId(const std::string& table_name) const {
    auto it = tables.find(table_name);
    if (it == tables.end()) {
        throw std::runtime_error("Table not found: " + table_name);
    }
    return it->second.table_id;
}

bool CatalogManager::hasTable(const std::string& table_name) const {
    return tables.find(table_name) != tables.end();
}

const std::unordered_map<std::string, TableMeta>& CatalogManager::getAllTables() const {
    return tables;
}

// persist table_id + schema definitions to storage
void CatalogManager::saveCatalog() {
    std::ofstream out(catalog_file, std::ios::trunc);
    for (const auto& [table_name, meta] : tables) {
        out << table_name << "|" << meta.table_id << "|" << meta.schema.serialize() << "\n";
    }
}

// Load table_id + schema definitions from catalog file
void CatalogManager::loadCatalog() {
    std::ifstream in(catalog_file);
    if (!in.is_open()) return;

    std::string line;
    while (std::getline(in, line)) {
        size_t first = line.find('|');
        if (first == std::string::npos) continue;
        size_t second = line.find('|', first + 1);
        if (second == std::string::npos) continue;

        std::string table_name = line.substr(0, first);
        uint32_t table_id = static_cast<uint32_t>(std::stoul(line.substr(first + 1, second - first - 1)));
        std::string serialized_schema = line.substr(second + 1);
        Schema schema = Schema::deserialize(serialized_schema);
        tables[table_name] = TableMeta{table_id, schema};
        if (table_id >= next_table_id) {
            next_table_id = table_id + 1;
        }
    }
}
