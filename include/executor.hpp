#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ast.hpp"
#include "database.hpp"

struct QueryResult {
    bool ok = true;
    std::string message;                         // "OK", or the error text when !ok
    std::vector<std::string> columns;             // SELECT column names (empty otherwise)
    std::vector<std::vector<std::string>> rows;   // SELECT projected rows
    int64_t affected_rows = 0;                    // INSERT/UPDATE/DELETE row count
};

// Binds and executes one parsed statement against `db`. `session_txn_id`
// is the calling session's currently-open transaction (0 = none, the same
// sentinel used everywhere below Database) -- BEGIN/COMMIT/ROLLBACK read
// AND mutate it directly here, so a connection (Session 4) doesn't have
// to duplicate that state machine itself; every other statement just
// passes it straight through to the Table call it binds to. Never
// throws: every failure (missing table, unknown column, a protocol error
// like COMMIT with nothing open) comes back as QueryResult{ok=false,
// message}, not an exception escaping to the caller.
QueryResult execute(const Stmt& stmt, Database& db, uint64_t& session_txn_id);
