#include "db_engine/database.hpp"

#include <filesystem>
#include <stdexcept>

#include "db_engine/recoverymanager.hpp"

namespace {
std::string ensureDir(const std::string& dir) {
    std::filesystem::create_directories(dir);
    return dir;
}

std::string joinPath(const std::string& dir, const std::string& leaf) {
    return dir + "/" + leaf;
}
}  // namespace

Database::Database(const std::string& data_dir)
    : data_dir(ensureDir(data_dir)), catalog(joinPath(this->data_dir, "catalog.db")) {
    wal = std::make_unique<WALWriter>(joinPath(this->data_dir, "wal.log"));
    txns = std::make_unique<TransactionManager>(*wal);

    // Recovery must run before any table's PageManager/IndexManager opens
    // its files for normal use (see RecoveryManager's own header comment)
    // -- gather every table the catalog already knows about (survives a
    // restart on its own, independent of the WAL) and replay against
    // their files.
    std::unordered_map<uint32_t, RecoveryManager::TableFiles> table_files;
    for (const auto& [name, meta] : catalog.getAllTables()) {
        table_files[meta.table_id] = RecoveryManager::TableFiles{
            heapFileFor(meta.table_id), indexFileFor(meta.table_id)};
    }
    RecoveryManager(*wal, table_files).run();

    mvcc = std::make_unique<MVCCManager>(*txns);

    for (const auto& [name, meta] : catalog.getAllTables()) {
        openTableStorage(name, meta.table_id, meta.schema);
    }
}

void Database::openTableStorage(const std::string& name, uint32_t table_id, const Schema& schema) {
    auto entry = std::make_unique<TableEntry>();
    entry->page_manager = std::make_unique<PageManager>(heapFileFor(table_id), *wal, *txns, table_id);
    entry->record_manager = std::make_unique<RecordManager>(*entry->page_manager);
    entry->index_manager = std::make_unique<IndexManager>(indexFileFor(table_id), *wal, *txns, table_id);
    entry->table = std::make_unique<Table>(name, schema, *entry->page_manager, *entry->record_manager,
                                            *entry->index_manager, *mvcc);
    tables[name] = std::move(entry);
}

void Database::createTable(const std::string& name, const Schema& schema) {
    uint32_t id = catalog.createTable(name, schema);  // throws if name already exists
    openTableStorage(name, id, schema);
}

Table& Database::getTable(const std::string& name) {
    auto it = tables.find(name);
    if (it == tables.end()) {
        throw std::runtime_error("Database::getTable: no such table: " + name);
    }
    return *it->second->table;
}

bool Database::hasTable(const std::string& name) const {
    return tables.find(name) != tables.end();
}

uint64_t Database::beginTxn() { return mvcc->begin(); }
void Database::commitTxn(uint64_t txn_id) { mvcc->commit(txn_id); }
void Database::abortTxn(uint64_t txn_id) { mvcc->abort(txn_id); }

std::string Database::heapFileFor(uint32_t table_id) const {
    return joinPath(data_dir, std::to_string(table_id) + ".heap");
}

std::string Database::indexFileFor(uint32_t table_id) const {
    return joinPath(data_dir, std::to_string(table_id) + ".idx");
}
