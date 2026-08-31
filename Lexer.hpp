// Lexer.hpp — Nova Language Lexer
// Implements Grammar 1 tokenization rules: dual brace/indentation scoping,
// comments (//, #, /* */), string & raw-string literals, all operators
// from the 25 Super-Syntax Extensions, and the full keyword dictionary.
#pragma once
#include "Token.hpp"
#include <string>
#include <vector>
#include <stdexcept>

namespace nova {

class LexError : public std::runtime_error {
public:
    LexError(const std::string& msg, int line, int col)
        : std::runtime_error("Lex error [line " + std::to_string(line) +
                              ", col " + std::to_string(col) + "]: " + msg),
          line(line), column(col) {}
    int line, column;
};

class Lexer {
public:
    explicit Lexer(std::string source);

    // Tokenizes the entire source and returns the flat token stream,
    // terminated by an END_OF_FILE token.
    std::vector<Token> tokenize();

private:
    std::string src;
    size_t pos = 0;
    int line = 1;
    int col = 1;

    // Indentation stack for Python-style blocks (Grammar 1: indent_block).
    std::vector<int> indentStack{0};

    // Depth of unmatched '(' / '[' — used to allow line-continuation
    // inside expressions (so multi-line calls/arrays don't need '\').
    int groupingDepth = 0;

    // True once we've emitted the first real token of the current logical
    // line (used to decide whether we're looking at pure leading whitespace).
    bool atLineStart = true;

    std::vector<Token> tokens;

    // ---- low level cursor helpers ----
    bool isAtEnd() const;
    char peek(int offset = 0) const;
    char advance();
    bool match(char expected);
    void addToken(TokenType type, const std::string& lexeme, int startLine, int startCol);

    // ---- major lexing routines ----
    void scanToken();
    void handleLeadingWhitespaceAndIndent();
    void scanIdentifierOrKeyword();
    void scanNumber();
    void scanString(char quote);
    void scanRawString();     // r""" ... """
    void skipLineComment();
    void skipBlockComment();

    void emitNewlineIfNeeded();
};

} // namespace nova
