#include "CatalogManager.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>

CatalogManager::CatalogManager(const std::string& catalog_file)
    : catalog_file(catalog_file) {
    loadCatalog(); //to in memory
}

void CatalogManager::createTable(const std::string& table_name, const Schema& schema) {
    if (hasTable(table_name)) {
        throw std::runtime_error("Table already exists: " + table_name);
    }

    tables[table_name] = schema;
    saveCatalog();
}

Schema CatalogManager::getSchema(const std::string& table_name) const {
    auto it = tables.find(table_name);
    if (it == tables.end()) {
        throw std::runtime_error("Table not found: " + table_name);
    }
    return it->second;
}

bool CatalogManager::hasTable(const std::string& table_name) const {
    return tables.find(table_name) != tables.end();
}

// persist schema definitions to storage
void CatalogManager::saveCatalog() {
    std::ofstream out(catalog_file, std::ios::trunc);
    for (const auto& [table_name, schema] : tables) {
        out << table_name << "|" << schema.serialize() << "\n";
    }
}

// Load schema definitions from catalog file
void CatalogManager::loadCatalog() {
    std::ifstream in(catalog_file);
    if (!in.is_open()) return;

    std::string line;
    while (std::getline(in, line)) {
        size_t delim = line.find('|');
        if (delim == std::string::npos) continue;

        std::string table_name = line.substr(0, delim);
        std::string serialized_schema = line.substr(delim + 1);
        Schema schema = Schema::deserialize(serialized_schema);
        tables[table_name] = schema;
    }
}
