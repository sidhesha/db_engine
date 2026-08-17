#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "catalogmanager.hpp"
#include "indexmanager.hpp"
#include "mvcc.hpp"
#include "pagemanager.hpp"
#include "recordmanager.hpp"
#include "table.hpp"
#include "wal.hpp"

// Owns everything a multi-table engine needs: one shared WAL/
// TransactionManager/MVCCManager (Phase 6) so a transaction spanning more
// than one table is still atomic and its snapshot/lock state is visible
// engine-wide, plus the CatalogManager and one PageManager+RecordManager+
// IndexManager+Table per table it knows about. Runs crash recovery once
// at construction, before any table's storage is opened for normal use --
// same ordering RecoveryManager's own header comment already documents
// for the single-table case.
class Database {
public:
    explicit Database(const std::string& data_dir);

    void createTable(const std::string& name, const Schema& schema);
    Table& getTable(const std::string& name);
    bool hasTable(const std::string& name) const;

    // Delegates straight to the one shared MVCCManager every Table also
    // references -- see table.hpp's constructor comment for why that has
    // to be shared rather than per-table.
    uint64_t beginTxn();
    void commitTxn(uint64_t txn_id);
    void abortTxn(uint64_t txn_id);

private:
    struct TableEntry {
        std::unique_ptr<PageManager> page_manager;
        std::unique_ptr<RecordManager> record_manager;
        std::unique_ptr<IndexManager> index_manager;
        std::unique_ptr<Table> table;
    };

    std::string data_dir;
    CatalogManager catalog;
    std::unique_ptr<WALWriter> wal;
    std::unique_ptr<TransactionManager> txns;
    std::unique_ptr<MVCCManager> mvcc;
    // Declared last so it's torn down FIRST: every TableEntry's
    // PageManager/IndexManager hold references into wal/txns/mvcc above,
    // which must still be alive while those destructors run (they flush
    // to disk on teardown).
    std::unordered_map<std::string, std::unique_ptr<TableEntry>> tables;

    std::string heapFileFor(uint32_t table_id) const;
    std::string indexFileFor(uint32_t table_id) const;
    void openTableStorage(const std::string& name, uint32_t table_id, const Schema& schema);
};
