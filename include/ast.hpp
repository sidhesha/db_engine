#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

enum class ComparisonOp { EQ, NEQ, LT, LTE, GT, GTE };

// Single-condition WHERE only (`col OP literal`) -- matches ROADMAP.md's
// Phase 6 scope; AND/OR, subqueries, and joins are explicitly out of
// scope for this phase. `literal` is the raw token text (executor
// coerces it to the target column's declared Schema type at bind time).
struct WhereClause {
    std::string column;
    ComparisonOp op;
    std::string literal;
};

struct ColumnDef {
    std::string name;
    std::string type;  // "int" | "string" -- validated by the executor, not the parser
    // Optional inert `PRIMARY KEY` marker for readability; the parser
    // only accepts it on column 0 (see parser.cpp), matching Table's
    // existing "first column is always the key" convention -- there's no
    // new storage-layer concept behind this syntax.
    bool primary_key = false;
};

struct CreateTableStmt {
    std::string table_name;
    std::vector<ColumnDef> columns;
};

struct InsertStmt {
    std::string table_name;
    std::vector<std::string> values;  // positional; must match the schema's column count/order
};

struct SelectStmt {
    std::string table_name;
    std::vector<std::string> columns;  // empty == SELECT *
    std::optional<WhereClause> where;
};

struct DeleteStmt {
    std::string table_name;
    std::optional<WhereClause> where;
};

struct Assignment {
    std::string column;
    std::string value;
};

struct UpdateStmt {
    std::string table_name;
    std::vector<Assignment> assignments;
    std::optional<WhereClause> where;
};

struct BeginStmt {};
struct CommitStmt {};
struct RollbackStmt {};

using Stmt = std::variant<CreateTableStmt, InsertStmt, SelectStmt, DeleteStmt, UpdateStmt,
                           BeginStmt, CommitStmt, RollbackStmt>;
