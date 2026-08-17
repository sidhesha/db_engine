#include "db_engine/parser.hpp"

Parser::Parser(std::vector<Token> tokens) : tokens(std::move(tokens)) {}

const Token& Parser::peek() const { return tokens[pos]; }

const Token& Parser::advance() {
    const Token& t = tokens[pos];
    if (pos + 1 < tokens.size()) pos++;  // never step past the trailing END_OF_INPUT sentinel
    return t;
}

bool Parser::check(SqlTokenType type) const { return peek().type == type; }

const Token& Parser::expect(SqlTokenType type, const std::string& what) {
    if (!check(type)) {
        throw ParseError("expected " + what + " but found '" + peek().text + "'", peek().pos);
    }
    return advance();
}

bool Parser::atEnd() const { return peek().type == SqlTokenType::END_OF_INPUT; }

std::string Parser::parseIdentifier() {
    if (!check(SqlTokenType::IDENTIFIER)) {
        throw ParseError("expected an identifier", peek().pos);
    }
    return advance().text;
}

std::string Parser::parseLiteral() {
    if (check(SqlTokenType::STRING_LITERAL) || check(SqlTokenType::NUMBER_LITERAL)) {
        return advance().text;
    }
    throw ParseError("expected a literal value", peek().pos);
}

ComparisonOp Parser::parseComparisonOp() {
    switch (peek().type) {
        case SqlTokenType::OP_EQ: advance(); return ComparisonOp::EQ;
        case SqlTokenType::OP_NEQ: advance(); return ComparisonOp::NEQ;
        case SqlTokenType::OP_LT: advance(); return ComparisonOp::LT;
        case SqlTokenType::OP_LTE: advance(); return ComparisonOp::LTE;
        case SqlTokenType::OP_GT: advance(); return ComparisonOp::GT;
        case SqlTokenType::OP_GTE: advance(); return ComparisonOp::GTE;
        default:
            throw ParseError("expected a comparison operator (= != < <= > >=)", peek().pos);
    }
}

WhereClause Parser::parseWhereClause() {
    WhereClause w;
    w.column = parseIdentifier();
    w.op = parseComparisonOp();
    w.literal = parseLiteral();
    return w;
}

Stmt Parser::parseStatement() {
    switch (peek().type) {
        case SqlTokenType::KEYWORD_CREATE: return parseCreateTable();
        case SqlTokenType::KEYWORD_INSERT: return parseInsert();
        case SqlTokenType::KEYWORD_SELECT: return parseSelect();
        case SqlTokenType::KEYWORD_DELETE: return parseDelete();
        case SqlTokenType::KEYWORD_UPDATE: return parseUpdate();
        case SqlTokenType::KEYWORD_BEGIN:
            advance();
            expect(SqlTokenType::SEMICOLON, "';'");
            return BeginStmt{};
        case SqlTokenType::KEYWORD_COMMIT:
            advance();
            expect(SqlTokenType::SEMICOLON, "';'");
            return CommitStmt{};
        case SqlTokenType::KEYWORD_ROLLBACK:
            advance();
            expect(SqlTokenType::SEMICOLON, "';'");
            return RollbackStmt{};
        default:
            throw ParseError(
                "expected a statement (CREATE/INSERT/SELECT/DELETE/UPDATE/BEGIN/COMMIT/ROLLBACK)",
                peek().pos);
    }
}

CreateTableStmt Parser::parseCreateTable() {
    expect(SqlTokenType::KEYWORD_CREATE, "CREATE");
    expect(SqlTokenType::KEYWORD_TABLE, "TABLE");

    CreateTableStmt stmt;
    stmt.table_name = parseIdentifier();
    expect(SqlTokenType::LPAREN, "'('");

    while (true) {
        ColumnDef col;
        col.name = parseIdentifier();
        col.type = parseIdentifier();
        if (check(SqlTokenType::KEYWORD_PRIMARY)) {
            std::size_t marker_pos = peek().pos;
            advance();
            expect(SqlTokenType::KEYWORD_KEY, "KEY");
            if (!stmt.columns.empty()) {
                throw ParseError("PRIMARY KEY is only supported on the first column", marker_pos);
            }
            col.primary_key = true;
        }
        stmt.columns.push_back(std::move(col));

        if (check(SqlTokenType::COMMA)) {
            advance();
            continue;
        }
        break;
    }

    expect(SqlTokenType::RPAREN, "')'");
    expect(SqlTokenType::SEMICOLON, "';'");
    return stmt;
}

InsertStmt Parser::parseInsert() {
    expect(SqlTokenType::KEYWORD_INSERT, "INSERT");
    expect(SqlTokenType::KEYWORD_INTO, "INTO");

    InsertStmt stmt;
    stmt.table_name = parseIdentifier();
    expect(SqlTokenType::KEYWORD_VALUES, "VALUES");
    expect(SqlTokenType::LPAREN, "'('");

    while (true) {
        stmt.values.push_back(parseLiteral());
        if (check(SqlTokenType::COMMA)) {
            advance();
            continue;
        }
        break;
    }

    expect(SqlTokenType::RPAREN, "')'");
    expect(SqlTokenType::SEMICOLON, "';'");
    return stmt;
}

SelectStmt Parser::parseSelect() {
    expect(SqlTokenType::KEYWORD_SELECT, "SELECT");

    SelectStmt stmt;
    if (check(SqlTokenType::STAR)) {
        advance();  // stmt.columns stays empty -- means '*'
    } else {
        stmt.columns.push_back(parseIdentifier());
        while (check(SqlTokenType::COMMA)) {
            advance();
            stmt.columns.push_back(parseIdentifier());
        }
    }

    expect(SqlTokenType::KEYWORD_FROM, "FROM");
    stmt.table_name = parseIdentifier();

    if (check(SqlTokenType::KEYWORD_WHERE)) {
        advance();
        stmt.where = parseWhereClause();
    }

    expect(SqlTokenType::SEMICOLON, "';'");
    return stmt;
}

DeleteStmt Parser::parseDelete() {
    expect(SqlTokenType::KEYWORD_DELETE, "DELETE");
    expect(SqlTokenType::KEYWORD_FROM, "FROM");

    DeleteStmt stmt;
    stmt.table_name = parseIdentifier();

    if (check(SqlTokenType::KEYWORD_WHERE)) {
        advance();
        stmt.where = parseWhereClause();
    }

    expect(SqlTokenType::SEMICOLON, "';'");
    return stmt;
}

UpdateStmt Parser::parseUpdate() {
    expect(SqlTokenType::KEYWORD_UPDATE, "UPDATE");

    UpdateStmt stmt;
    stmt.table_name = parseIdentifier();
    expect(SqlTokenType::KEYWORD_SET, "SET");

    while (true) {
        Assignment a;
        a.column = parseIdentifier();
        expect(SqlTokenType::OP_EQ, "'='");
        a.value = parseLiteral();
        stmt.assignments.push_back(std::move(a));

        if (check(SqlTokenType::COMMA)) {
            advance();
            continue;
        }
        break;
    }

    if (check(SqlTokenType::KEYWORD_WHERE)) {
        advance();
        stmt.where = parseWhereClause();
    }

    expect(SqlTokenType::SEMICOLON, "';'");
    return stmt;
}
