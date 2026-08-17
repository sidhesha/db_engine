#include "lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace {
const std::unordered_map<std::string, SqlTokenType>& keywordTable() {
    static const std::unordered_map<std::string, SqlTokenType> table = {
        {"CREATE", SqlTokenType::KEYWORD_CREATE}, {"TABLE", SqlTokenType::KEYWORD_TABLE},
        {"INSERT", SqlTokenType::KEYWORD_INSERT}, {"INTO", SqlTokenType::KEYWORD_INTO},
        {"VALUES", SqlTokenType::KEYWORD_VALUES}, {"SELECT", SqlTokenType::KEYWORD_SELECT},
        {"FROM", SqlTokenType::KEYWORD_FROM}, {"WHERE", SqlTokenType::KEYWORD_WHERE},
        {"DELETE", SqlTokenType::KEYWORD_DELETE}, {"UPDATE", SqlTokenType::KEYWORD_UPDATE},
        {"SET", SqlTokenType::KEYWORD_SET}, {"BEGIN", SqlTokenType::KEYWORD_BEGIN},
        {"COMMIT", SqlTokenType::KEYWORD_COMMIT}, {"ROLLBACK", SqlTokenType::KEYWORD_ROLLBACK},
        {"PRIMARY", SqlTokenType::KEYWORD_PRIMARY}, {"KEY", SqlTokenType::KEYWORD_KEY},
    };
    return table;
}

std::string toUpper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
}  // namespace

Lexer::Lexer(std::string source) : source(std::move(source)) {}

bool Lexer::atEnd() const { return pos >= source.size(); }

char Lexer::peek() const { return atEnd() ? '\0' : source[pos]; }

char Lexer::peekNext() const { return (pos + 1 >= source.size()) ? '\0' : source[pos + 1]; }

char Lexer::advance() { return source[pos++]; }

void Lexer::skipWhitespaceAndComments() {
    while (!atEnd()) {
        char c = peek();
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
        } else if (c == '-' && peekNext() == '-') {
            // `-- ...` line comment, standard SQL, purely a lexer-level
            // convenience -- discarded here so the parser never sees it.
            while (!atEnd() && peek() != '\n') advance();
        } else {
            break;
        }
    }
}

Token Lexer::lexIdentifierOrKeyword() {
    std::size_t start = pos;
    while (!atEnd() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) advance();
    std::string text = source.substr(start, pos - start);
    std::string upper = toUpper(text);
    auto it = keywordTable().find(upper);
    if (it != keywordTable().end()) {
        return Token{it->second, upper, start};
    }
    return Token{SqlTokenType::IDENTIFIER, text, start};
}

Token Lexer::lexNumber() {
    std::size_t start = pos;
    if (peek() == '-') advance();
    while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    return Token{SqlTokenType::NUMBER_LITERAL, source.substr(start, pos - start), start};
}

Token Lexer::lexString() {
    std::size_t start = pos;
    advance();  // opening '
    std::string value;
    while (true) {
        if (atEnd()) {
            throw LexError("unterminated string literal", start);
        }
        char c = advance();
        if (c == '\'') {
            if (peek() == '\'') {  // '' -- escaped quote inside the literal
                value.push_back('\'');
                advance();
                continue;
            }
            break;  // closing quote
        }
        value.push_back(c);
    }
    return Token{SqlTokenType::STRING_LITERAL, value, start};
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        skipWhitespaceAndComments();
        if (atEnd()) {
            tokens.push_back(Token{SqlTokenType::END_OF_INPUT, "", pos});
            break;
        }

        char c = peek();
        std::size_t start = pos;

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            tokens.push_back(lexIdentifierOrKeyword());
        } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                   (c == '-' && std::isdigit(static_cast<unsigned char>(peekNext())))) {
            tokens.push_back(lexNumber());
        } else if (c == '\'') {
            tokens.push_back(lexString());
        } else if (c == '=') {
            advance();
            tokens.push_back(Token{SqlTokenType::OP_EQ, "=", start});
        } else if (c == '<') {
            advance();
            if (peek() == '=') { advance(); tokens.push_back(Token{SqlTokenType::OP_LTE, "<=", start}); }
            else if (peek() == '>') { advance(); tokens.push_back(Token{SqlTokenType::OP_NEQ, "<>", start}); }
            else { tokens.push_back(Token{SqlTokenType::OP_LT, "<", start}); }
        } else if (c == '>') {
            advance();
            if (peek() == '=') { advance(); tokens.push_back(Token{SqlTokenType::OP_GTE, ">=", start}); }
            else { tokens.push_back(Token{SqlTokenType::OP_GT, ">", start}); }
        } else if (c == '!') {
            advance();
            if (peek() != '=') throw LexError("expected '=' after '!'", start);
            advance();
            tokens.push_back(Token{SqlTokenType::OP_NEQ, "!=", start});
        } else if (c == ',') {
            advance();
            tokens.push_back(Token{SqlTokenType::COMMA, ",", start});
        } else if (c == '(') {
            advance();
            tokens.push_back(Token{SqlTokenType::LPAREN, "(", start});
        } else if (c == ')') {
            advance();
            tokens.push_back(Token{SqlTokenType::RPAREN, ")", start});
        } else if (c == '*') {
            advance();
            tokens.push_back(Token{SqlTokenType::STAR, "*", start});
        } else if (c == ';') {
            advance();
            tokens.push_back(Token{SqlTokenType::SEMICOLON, ";", start});
        } else {
            throw LexError(std::string("unrecognized character '") + c + "'", start);
        }
    }
    return tokens;
}
