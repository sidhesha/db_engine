#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& msg, std::size_t pos)
        : std::runtime_error(msg + " (at position " + std::to_string(pos) + ")") {}
};

// Recursive-descent parser over a Lexer's token stream. One entry point
// per statement kind; parseStatement() dispatches on the leading keyword
// and consumes (and requires) that statement's trailing ';'. A buffer
// holding multiple statements is parsed by calling parseStatement()
// repeatedly until atEnd().
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    Stmt parseStatement();
    bool atEnd() const;

private:
    std::vector<Token> tokens;
    std::size_t pos = 0;

    const Token& peek() const;
    const Token& advance();
    const Token& expect(SqlTokenType type, const std::string& what);
    bool check(SqlTokenType type) const;

    CreateTableStmt parseCreateTable();
    InsertStmt parseInsert();
    SelectStmt parseSelect();
    DeleteStmt parseDelete();
    UpdateStmt parseUpdate();

    std::string parseIdentifier();
    std::string parseLiteral();  // STRING_LITERAL or NUMBER_LITERAL -> raw text
    WhereClause parseWhereClause();
    ComparisonOp parseComparisonOp();
};
