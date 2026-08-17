#include "db_engine/executor.hpp"

#include <cctype>
#include <stdexcept>

namespace {

std::string normalizeType(const std::string& raw) {
    std::string t = raw;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t;
}

int columnIndex(const Schema& schema, const std::string& name) {
    const auto& cols = schema.getColumns();
    for (std::size_t i = 0; i < cols.size(); i++) {
        if (cols[i].name == name) return static_cast<int>(i);
    }
    throw std::runtime_error("no such column: " + name);
}

// Type-aware WHERE evaluation: an "int" column's literal and field value
// are compared numerically (`age > 5` must not treat "10" < "9" the way
// a lexicographic compare would); every other declared type compares as
// plain strings.
bool evalWhere(const std::string& field_value, ComparisonOp op, const std::string& literal,
               const std::string& column_type) {
    if (normalizeType(column_type) == "int") {
        long long a = std::stoll(field_value);
        long long b = std::stoll(literal);
        switch (op) {
            case ComparisonOp::EQ: return a == b;
            case ComparisonOp::NEQ: return a != b;
            case ComparisonOp::LT: return a < b;
            case ComparisonOp::LTE: return a <= b;
            case ComparisonOp::GT: return a > b;
            case ComparisonOp::GTE: return a >= b;
        }
        throw std::logic_error("evalWhere: unreachable (int)");
    }
    int cmp = field_value.compare(literal);
    switch (op) {
        case ComparisonOp::EQ: return cmp == 0;
        case ComparisonOp::NEQ: return cmp != 0;
        case ComparisonOp::LT: return cmp < 0;
        case ComparisonOp::LTE: return cmp <= 0;
        case ComparisonOp::GT: return cmp > 0;
        case ComparisonOp::GTE: return cmp >= 0;
    }
    throw std::logic_error("evalWhere: unreachable (string)");
}

// True if `where` targets the schema's first (primary key) column with
// equality -- Table's getByKey/updateByKey/deleteByKey fast path. Any
// other column or operator has no index to use and needs scanAll().
bool isPrimaryKeyEquality(const Schema& schema, const WhereClause& where) {
    return where.op == ComparisonOp::EQ && !schema.getColumns().empty() &&
           schema.getColumns()[0].name == where.column;
}

QueryResult execCreateTable(const CreateTableStmt& stmt, Database& db) {
    std::vector<Column> columns;
    for (const auto& col : stmt.columns) {
        std::string type = normalizeType(col.type);
        if (type != "int" && type != "string") {
            throw std::runtime_error("unknown column type '" + col.type + "' (expected int or string)");
        }
        columns.emplace_back(col.name, type);
    }
    db.createTable(stmt.table_name, Schema(columns));
    return QueryResult{};
}

QueryResult execInsert(const InsertStmt& stmt, Database& db, uint64_t txn_id) {
    Table& table = db.getTable(stmt.table_name);
    table.insert(stmt.values, txn_id);
    QueryResult r;
    r.affected_rows = 1;
    return r;
}

QueryResult execSelect(const SelectStmt& stmt, Database& db, uint64_t txn_id) {
    Table& table = db.getTable(stmt.table_name);
    const Schema& schema = table.getSchema();
    const auto& all_columns = schema.getColumns();

    std::vector<int> projection;  // indices into a row's fields
    if (stmt.columns.empty()) {
        for (std::size_t i = 0; i < all_columns.size(); i++) projection.push_back(static_cast<int>(i));
    } else {
        for (const auto& name : stmt.columns) projection.push_back(columnIndex(schema, name));
    }

    QueryResult r;
    for (int idx : projection) r.columns.push_back(all_columns[idx].name);

    auto project = [&](const Record& rec) {
        const std::vector<std::string> fields = rec.getFields();
        std::vector<std::string> row;
        row.reserve(projection.size());
        for (int idx : projection) row.push_back(fields[idx]);
        r.rows.push_back(std::move(row));
    };

    if (stmt.where.has_value() && isPrimaryKeyEquality(schema, *stmt.where)) {
        auto rec = table.getByKey(stmt.where->literal, txn_id);
        if (rec.has_value()) project(*rec);
    } else {
        int where_idx = stmt.where.has_value() ? columnIndex(schema, stmt.where->column) : -1;
        for (auto& [key, rec] : table.scanAll(txn_id)) {
            (void)key;
            if (stmt.where.has_value() &&
                !evalWhere(rec.getFields()[where_idx], stmt.where->op, stmt.where->literal,
                           all_columns[where_idx].type)) {
                continue;
            }
            project(rec);
        }
    }
    return r;
}

QueryResult execDelete(const DeleteStmt& stmt, Database& db, uint64_t txn_id) {
    Table& table = db.getTable(stmt.table_name);
    const Schema& schema = table.getSchema();

    int64_t count = 0;
    if (stmt.where.has_value() && isPrimaryKeyEquality(schema, *stmt.where)) {
        if (table.deleteByKey(stmt.where->literal, txn_id)) count = 1;
    } else {
        int where_idx = stmt.where.has_value() ? columnIndex(schema, stmt.where->column) : -1;
        std::vector<std::string> keys_to_delete;
        for (auto& [key, rec] : table.scanAll(txn_id)) {
            if (stmt.where.has_value() &&
                !evalWhere(rec.getFields()[where_idx], stmt.where->op, stmt.where->literal,
                           schema.getColumns()[where_idx].type)) {
                continue;
            }
            keys_to_delete.push_back(key.toString());
        }
        // Deletions applied after the scan finishes, not interleaved with
        // it -- scanAll() already captured a consistent snapshot's worth
        // of matching keys, and deleteByKey() only ever tombstones (never
        // removes the index entry), so there's no risk of a delete
        // shifting keys out from under the still-iterating scan.
        for (const auto& k : keys_to_delete) {
            if (table.deleteByKey(k, txn_id)) count++;
        }
    }

    QueryResult r;
    r.affected_rows = count;
    return r;
}

QueryResult execUpdate(const UpdateStmt& stmt, Database& db, uint64_t txn_id) {
    Table& table = db.getTable(stmt.table_name);
    const Schema& schema = table.getSchema();

    auto applyAssignments = [&](std::vector<std::string> fields) {
        for (const auto& a : stmt.assignments) {
            fields[columnIndex(schema, a.column)] = a.value;
        }
        return fields;
    };

    int64_t count = 0;
    if (stmt.where.has_value() && isPrimaryKeyEquality(schema, *stmt.where)) {
        auto rec = table.getByKey(stmt.where->literal, txn_id);
        if (rec.has_value()) {
            if (table.updateByKey(stmt.where->literal, applyAssignments(rec->getFields()), txn_id)) {
                count = 1;
            }
        }
    } else {
        int where_idx = stmt.where.has_value() ? columnIndex(schema, stmt.where->column) : -1;
        struct Pending { std::string key; std::vector<std::string> fields; };
        std::vector<Pending> pending;
        for (auto& [key, rec] : table.scanAll(txn_id)) {
            if (stmt.where.has_value() &&
                !evalWhere(rec.getFields()[where_idx], stmt.where->op, stmt.where->literal,
                           schema.getColumns()[where_idx].type)) {
                continue;
            }
            pending.push_back(Pending{key.toString(), applyAssignments(rec.getFields())});
        }
        for (auto& p : pending) {
            if (table.updateByKey(p.key, p.fields, txn_id)) count++;
        }
    }

    QueryResult r;
    r.affected_rows = count;
    return r;
}

QueryResult execBegin(uint64_t& session_txn_id, Database& db) {
    if (session_txn_id != 0) {
        throw std::runtime_error("a transaction is already in progress on this connection");
    }
    session_txn_id = db.beginTxn();
    return QueryResult{};
}

QueryResult execCommit(uint64_t& session_txn_id, Database& db) {
    if (session_txn_id == 0) {
        throw std::runtime_error("no transaction is in progress on this connection");
    }
    db.commitTxn(session_txn_id);
    session_txn_id = 0;
    return QueryResult{};
}

QueryResult execRollback(uint64_t& session_txn_id, Database& db) {
    if (session_txn_id == 0) {
        throw std::runtime_error("no transaction is in progress on this connection");
    }
    db.abortTxn(session_txn_id);
    session_txn_id = 0;
    return QueryResult{};
}

}  // namespace

QueryResult execute(const Stmt& stmt, Database& db, uint64_t& session_txn_id) {
    try {
        if (std::holds_alternative<CreateTableStmt>(stmt)) {
            return execCreateTable(std::get<CreateTableStmt>(stmt), db);
        }
        if (std::holds_alternative<InsertStmt>(stmt)) {
            return execInsert(std::get<InsertStmt>(stmt), db, session_txn_id);
        }
        if (std::holds_alternative<SelectStmt>(stmt)) {
            return execSelect(std::get<SelectStmt>(stmt), db, session_txn_id);
        }
        if (std::holds_alternative<DeleteStmt>(stmt)) {
            return execDelete(std::get<DeleteStmt>(stmt), db, session_txn_id);
        }
        if (std::holds_alternative<UpdateStmt>(stmt)) {
            return execUpdate(std::get<UpdateStmt>(stmt), db, session_txn_id);
        }
        if (std::holds_alternative<BeginStmt>(stmt)) {
            return execBegin(session_txn_id, db);
        }
        if (std::holds_alternative<CommitStmt>(stmt)) {
            return execCommit(session_txn_id, db);
        }
        return execRollback(session_txn_id, db);
    } catch (const std::exception& e) {
        QueryResult r;
        r.ok = false;
        r.message = e.what();
        return r;
    }
}
