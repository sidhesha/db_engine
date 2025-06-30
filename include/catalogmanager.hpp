#pragma once

#include "Schema.hpp"
#include <unordered_map>
#include <string>
#include <fstream>

class CatalogManager {
public:
    CatalogManager(const std::string& catalog_file);
    
    void createTable(const std::string& table_name, const Schema& schema);
    Schema getSchema(const std::string& table_name) const;
    bool hasTable(const std::string& table_name) const;

    void loadCatalog();
    void saveCatalog();

private:
    std::string catalog_file;
    std::unordered_map<std::string, Schema> tables;
};