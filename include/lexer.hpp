#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

enum class TokenType {
    KEYWORD_CREATE, KEYWORD_TABLE, KEYWORD_INSERT, KEYWORD_INTO, KEYWORD_VALUES,
    KEYWORD_SELECT, KEYWORD_FROM, KEYWORD_WHERE, KEYWORD_DELETE, KEYWORD_UPDATE,
    KEYWORD_SET, KEYWORD_BEGIN, KEYWORD_COMMIT, KEYWORD_ROLLBACK,
    KEYWORD_PRIMARY, KEYWORD_KEY,
    IDENTIFIER, STRING_LITERAL, NUMBER_LITERAL,
    OP_EQ, OP_NEQ, OP_LT, OP_LTE, OP_GT, OP_GTE,
    COMMA, LPAREN, RPAREN, STAR, SEMICOLON,
    END_OF_INPUT,
};

struct Token {
    TokenType type;
    std::string text;   // original text (identifiers/literals keep their case; keywords normalized upper)
    std::size_t pos;     // byte offset into the source, for error messages
};

class LexError : public std::runtime_error {
public:
    LexError(const std::string& msg, std::size_t pos)
        : std::runtime_error(msg + " (at position " + std::to_string(pos) + ")") {}
};

// Tokenizes one or more `;`-terminated SQL statements eagerly into a flat
// vector (statements are short -- no need to stream). Keywords are
// matched case-insensitively; identifier/string-literal text keeps its
// original case. Throws LexError on an unterminated string literal, a
// stray '!' not followed by '=', or any other unrecognized character.
class Lexer {
public:
    explicit Lexer(std::string source);
    std::vector<Token> tokenize();

private:
    std::string source;
    std::size_t pos = 0;

    char peek() const;
    char peekNext() const;
    char advance();
    bool atEnd() const;
    void skipWhitespaceAndComments();

    Token lexIdentifierOrKeyword();
    Token lexNumber();
    Token lexString();
};
