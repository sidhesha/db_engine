#include "parser.hpp"

Parser::Parser(std::vector<Token> tokens) : tokens(std::move(tokens)) {}

const Token& Parser::peek() const { return tokens[pos]; }

const Token& Parser::advance() {
    const Token& t = tokens[pos];
    if (pos + 1 < tokens.size()) pos++;  // never step past the trailing END_OF_INPUT sentinel
    return t;
}

bool Parser::check(TokenType type) const { return peek().type == type; }

const Token& Parser::expect(TokenType type, const std::string& what) {
    if (!check(type)) {
        throw ParseError("expected " + what + " but found '" + peek().text + "'", peek().pos);
    }
    return advance();
}

bool Parser::atEnd() const { return peek().type == TokenType::END_OF_INPUT; }

std::string Parser::parseIdentifier() {
    if (!check(TokenType::IDENTIFIER)) {
        throw ParseError("expected an identifier", peek().pos);
    }
    return advance().text;
}

std::string Parser::parseLiteral() {
    if (check(TokenType::STRING_LITERAL) || check(TokenType::NUMBER_LITERAL)) {
        return advance().text;
    }
    throw ParseError("expected a literal value", peek().pos);
}

ComparisonOp Parser::parseComparisonOp() {
    switch (peek().type) {
        case TokenType::OP_EQ: advance(); return ComparisonOp::EQ;
        case TokenType::OP_NEQ: advance(); return ComparisonOp::NEQ;
        case TokenType::OP_LT: advance(); return ComparisonOp::LT;
        case TokenType::OP_LTE: advance(); return ComparisonOp::LTE;
        case TokenType::OP_GT: advance(); return ComparisonOp::GT;
        case TokenType::OP_GTE: advance(); return ComparisonOp::GTE;
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
        case TokenType::KEYWORD_CREATE: return parseCreateTable();
        case TokenType::KEYWORD_INSERT: return parseInsert();
        case TokenType::KEYWORD_SELECT: return parseSelect();
        case TokenType::KEYWORD_DELETE: return parseDelete();
        case TokenType::KEYWORD_UPDATE: return parseUpdate();
        case TokenType::KEYWORD_BEGIN:
            advance();
            expect(TokenType::SEMICOLON, "';'");
            return BeginStmt{};
        case TokenType::KEYWORD_COMMIT:
            advance();
            expect(TokenType::SEMICOLON, "';'");
            return CommitStmt{};
        case TokenType::KEYWORD_ROLLBACK:
            advance();
            expect(TokenType::SEMICOLON, "';'");
            return RollbackStmt{};
        default:
            throw ParseError(
                "expected a statement (CREATE/INSERT/SELECT/DELETE/UPDATE/BEGIN/COMMIT/ROLLBACK)",
                peek().pos);
    }
}

CreateTableStmt Parser::parseCreateTable() {
    expect(TokenType::KEYWORD_CREATE, "CREATE");
    expect(TokenType::KEYWORD_TABLE, "TABLE");

    CreateTableStmt stmt;
    stmt.table_name = parseIdentifier();
    expect(TokenType::LPAREN, "'('");

    while (true) {
        ColumnDef col;
        col.name = parseIdentifier();
        col.type = parseIdentifier();
        if (check(TokenType::KEYWORD_PRIMARY)) {
            std::size_t marker_pos = peek().pos;
            advance();
            expect(TokenType::KEYWORD_KEY, "KEY");
            if (!stmt.columns.empty()) {
                throw ParseError("PRIMARY KEY is only supported on the first column", marker_pos);
            }
            col.primary_key = true;
        }
        stmt.columns.push_back(std::move(col));

        if (check(TokenType::COMMA)) {
            advance();
            continue;
        }
        break;
    }

    expect(TokenType::RPAREN, "')'");
    expect(TokenType::SEMICOLON, "';'");
    return stmt;
}

InsertStmt Parser::parseInsert() {
    expect(TokenType::KEYWORD_INSERT, "INSERT");
    expect(TokenType::KEYWORD_INTO, "INTO");

    InsertStmt stmt;
    stmt.table_name = parseIdentifier();
    expect(TokenType::KEYWORD_VALUES, "VALUES");
    expect(TokenType::LPAREN, "'('");

    while (true) {
        stmt.values.push_back(parseLiteral());
        if (check(TokenType::COMMA)) {
            advance();
            continue;
        }
        break;
    }

    expect(TokenType::RPAREN, "')'");
    expect(TokenType::SEMICOLON, "';'");
    return stmt;
}

SelectStmt Parser::parseSelect() {
    expect(TokenType::KEYWORD_SELECT, "SELECT");

    SelectStmt stmt;
    if (check(TokenType::STAR)) {
        advance();  // stmt.columns stays empty -- means '*'
    } else {
        stmt.columns.push_back(parseIdentifier());
        while (check(TokenType::COMMA)) {
            advance();
            stmt.columns.push_back(parseIdentifier());
        }
    }

    expect(TokenType::KEYWORD_FROM, "FROM");
    stmt.table_name = parseIdentifier();

    if (check(TokenType::KEYWORD_WHERE)) {
        advance();
        stmt.where = parseWhereClause();
    }

    expect(TokenType::SEMICOLON, "';'");
    return stmt;
}

DeleteStmt Parser::parseDelete() {
    expect(TokenType::KEYWORD_DELETE, "DELETE");
    expect(TokenType::KEYWORD_FROM, "FROM");

    DeleteStmt stmt;
    stmt.table_name = parseIdentifier();

    if (check(TokenType::KEYWORD_WHERE)) {
        advance();
        stmt.where = parseWhereClause();
    }

    expect(TokenType::SEMICOLON, "';'");
    return stmt;
}

UpdateStmt Parser::parseUpdate() {
    expect(TokenType::KEYWORD_UPDATE, "UPDATE");

    UpdateStmt stmt;
    stmt.table_name = parseIdentifier();
    expect(TokenType::KEYWORD_SET, "SET");

    while (true) {
        Assignment a;
        a.column = parseIdentifier();
        expect(TokenType::OP_EQ, "'='");
        a.value = parseLiteral();
        stmt.assignments.push_back(std::move(a));

        if (check(TokenType::COMMA)) {
            advance();
            continue;
        }
        break;
    }

    if (check(TokenType::KEYWORD_WHERE)) {
        advance();
        stmt.where = parseWhereClause();
    }

    expect(TokenType::SEMICOLON, "';'");
    return stmt;
}
