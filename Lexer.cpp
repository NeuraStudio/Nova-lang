// Lexer.cpp — Nova Language Lexer implementation
#include "Lexer.hpp"
#include <cctype>

namespace nova {

Lexer::Lexer(std::string source) : src(std::move(source)) {}

bool Lexer::isAtEnd() const { return pos >= src.size(); }

char Lexer::peek(int offset) const {
    size_t p = pos + offset;
    if (p >= src.size()) return '\0';
    return src[p];
}

char Lexer::advance() {
    char c = src[pos++];
    if (c == '\n') { line++; col = 1; }
    else { col++; }
    return c;
}

bool Lexer::match(char expected) {
    if (isAtEnd() || src[pos] != expected) return false;
    advance();
    return true;
}

void Lexer::addToken(TokenType type, const std::string& lexeme, int startLine, int startCol) {
    tokens.emplace_back(type, lexeme, startLine, startCol);
}

// Emits a NEWLINE token representing the end of a logical line, but only
// if the previous token isn't already NEWLINE/INDENT/DEDENT (avoids noise
// on blank lines) and we actually have prior tokens.
void Lexer::emitNewlineIfNeeded() {
    if (tokens.empty()) return;
    TokenType last = tokens.back().type;
    if (last == TokenType::NEWLINE || last == TokenType::INDENT || last == TokenType::DEDENT) return;
    addToken(TokenType::NEWLINE, "\\n", line, col);
}

// Handles leading whitespace at the start of a logical line (groupingDepth==0):
// computes indentation width and pushes INDENT / pops DEDENT tokens to match
// Python-style hybrid blocks (Grammar 1: indent_block = ':' NEWLINE INDENT ... DEDENT).
void Lexer::handleLeadingWhitespaceAndIndent() {
    int width = 0;
    size_t save = pos;
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ') { width++; advance(); }
        else if (c == '\t') { width += 8; advance(); }
        else break;
    }
    (void)save;

    // Blank line or comment-only line: don't touch indent stack, caller
    // will detect newline/comment and loop again.
    char c = peek();
    if (c == '\n' || c == '\0' || c == '\r') return;
    if (c == '/' && peek(1) == '/') return;
    if (c == '#') return;
    if (c == '/' && peek(1) == '*') return;

    int current = indentStack.back();
    if (width > current) {
        indentStack.push_back(width);
        addToken(TokenType::INDENT, "<indent>", line, col);
    } else if (width < current) {
        while (!indentStack.empty() && indentStack.back() > width) {
            indentStack.pop_back();
            addToken(TokenType::DEDENT, "<dedent>", line, col);
        }
        // Tolerate slightly mismatched dedents (lenient recovery) by
        // resyncing the stack top to the observed width.
        if (indentStack.empty() || indentStack.back() != width) {
            indentStack.push_back(width);
        }
    }
}

void Lexer::scanIdentifierOrKeyword() {
    int startLine = line, startCol = col;
    size_t start = pos;
    while (!isAtEnd() && (std::isalnum((unsigned char)peek()) || peek() == '_')) advance();
    std::string text = src.substr(start, pos - start);

    // "r\"\"\"" raw string prefix check handled before calling this (scanToken).
    const auto& kw = keywordTable();
    auto it = kw.find(text);
    if (it != kw.end()) {
        addToken(it->second, text, startLine, startCol);
    } else {
        addToken(TokenType::IDENTIFIER, text, startLine, startCol);
    }
}

void Lexer::scanNumber() {
    int startLine = line, startCol = col;
    size_t start = pos;
    bool isFloat = false;
    while (!isAtEnd() && std::isdigit((unsigned char)peek())) advance();
    if (peek() == '.' && std::isdigit((unsigned char)peek(1))) {
        isFloat = true;
        advance(); // consume '.'
        while (!isAtEnd() && std::isdigit((unsigned char)peek())) advance();
    }
    // scientific notation e.g. 1e10, 2.5e-3
    if ((peek() == 'e' || peek() == 'E') &&
        (std::isdigit((unsigned char)peek(1)) ||
         ((peek(1) == '+' || peek(1) == '-') && std::isdigit((unsigned char)peek(2))))) {
        isFloat = true;
        advance();
        if (peek() == '+' || peek() == '-') advance();
        while (!isAtEnd() && std::isdigit((unsigned char)peek())) advance();
    }
    std::string text = src.substr(start, pos - start);
    if (isFloat) {
        Token t(TokenType::FLOAT_LITERAL, text, startLine, startCol);
        t.floatValue = std::stod(text);
        tokens.push_back(t);
    } else {
        Token t(TokenType::INT_LITERAL, text, startLine, startCol);
        t.intValue = std::stoll(text);
        tokens.push_back(t);
    }
}

void Lexer::scanString(char quote) {
    int startLine = line, startCol = col;
    advance(); // consume opening quote
    std::string value;
    while (!isAtEnd() && peek() != quote) {
        char c = peek();
        if (c == '\\') {
            advance();
            char esc = peek();
            switch (esc) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                case '\'': value += '\''; break;
                case '0': value += '\0'; break;
                default: value += esc; break;
            }
            advance();
        } else {
            value += c;
            advance();
        }
    }
    if (isAtEnd()) throw LexError("unterminated string literal", startLine, startCol);
    advance(); // consume closing quote
    Token t(TokenType::STRING_LITERAL, value, startLine, startCol);
    tokens.push_back(t);
}

// Raw multi-line strings: r""" ... """ (Extension #9)
void Lexer::scanRawString() {
    int startLine = line, startCol = col;
    advance(); advance(); advance(); advance(); // consume r + """
    std::string value;
    while (!isAtEnd() && !(peek() == '"' && peek(1) == '"' && peek(2) == '"')) {
        value += advance();
    }
    if (isAtEnd()) throw LexError("unterminated raw string literal (r\"\"\" ... \"\"\")", startLine, startCol);
    advance(); advance(); advance(); // consume closing """
    Token t(TokenType::RAW_STRING_LITERAL, value, startLine, startCol);
    tokens.push_back(t);
}

void Lexer::skipLineComment() {
    while (!isAtEnd() && peek() != '\n') advance();
}

void Lexer::skipBlockComment() {
    advance(); advance(); // consume /*
    while (!isAtEnd() && !(peek() == '*' && peek(1) == '/')) advance();
    if (!isAtEnd()) { advance(); advance(); } // consume */
}

void Lexer::scanToken() {
    char c = peek();

    // --- Whitespace (non-newline) ---
    if (c == ' ' || c == '\t' || c == '\r') { advance(); return; }

    // --- Newline: logical line end ---
    if (c == '\n') {
        advance();
        if (groupingDepth == 0) {
            emitNewlineIfNeeded();
            handleLeadingWhitespaceAndIndent();
        }
        return;
    }

    // --- Comments ---
    if (c == '/' && peek(1) == '/') { skipLineComment(); return; }
    if (c == '/' && peek(1) == '*') { skipBlockComment(); return; }
    if (c == '#') { skipLineComment(); return; }

    // --- Raw string literal: r""" ... """ ---
    if (c == 'r' && peek(1) == '"' && peek(2) == '"' && peek(3) == '"') {
        scanRawString();
        return;
    }

    // --- Identifiers / keywords ---
    if (std::isalpha((unsigned char)c) || c == '_') { scanIdentifierOrKeyword(); return; }

    // --- Numbers ---
    if (std::isdigit((unsigned char)c)) { scanNumber(); return; }

    // --- Strings ---
    if (c == '"' || c == '\'') { scanString(c); return; }

    // --- Lambda shorthand params $0 $1 (Extension #10) ---
    if (c == '$' && std::isdigit((unsigned char)peek(1))) {
        int startLine = line, startCol = col;
        size_t start = pos;
        advance(); // $
        while (!isAtEnd() && std::isdigit((unsigned char)peek())) advance();
        addToken(TokenType::DOLLAR_PARAM, src.substr(start, pos - start), startLine, startCol);
        return;
    }

    int startLine = line, startCol = col;

    switch (c) {
        case '(': advance(); groupingDepth++; addToken(TokenType::LPAREN, "(", startLine, startCol); return;
        case ')': advance(); if (groupingDepth > 0) groupingDepth--; addToken(TokenType::RPAREN, ")", startLine, startCol); return;
        case '[': advance(); groupingDepth++; addToken(TokenType::LBRACKET, "[", startLine, startCol); return;
        case ']': advance(); if (groupingDepth > 0) groupingDepth--; addToken(TokenType::RBRACKET, "]", startLine, startCol); return;
        case '{': advance(); addToken(TokenType::LBRACE, "{", startLine, startCol); return;
        case '}': advance(); addToken(TokenType::RBRACE, "}", startLine, startCol); return;
        case ',': advance(); addToken(TokenType::COMMA, ",", startLine, startCol); return;
        case ';': advance(); addToken(TokenType::SEMICOLON, ";", startLine, startCol); return;
        case '@': advance(); addToken(TokenType::AT, "@", startLine, startCol); return;
        case '~': advance(); addToken(TokenType::TILDE, "~", startLine, startCol); return;

        case '.':
            advance();
            if (peek() == '.' && peek(1) == '.') { advance(); advance(); addToken(TokenType::ELLIPSIS, "...", startLine, startCol); return; }
            if (peek() == '.') { advance(); addToken(TokenType::RANGE, "..", startLine, startCol); return; }
            addToken(TokenType::DOT, ".", startLine, startCol);
            return;

        case ':':
            advance();
            addToken(TokenType::COLON, ":", startLine, startCol);
            return;

        case '=':
            advance();
            if (match('=')) { addToken(TokenType::EQ, "==", startLine, startCol); return; }
            if (match('>')) { addToken(TokenType::ARROW_FAT, "=>", startLine, startCol); return; }
            addToken(TokenType::ASSIGN, "=", startLine, startCol);
            return;

        case '!':
            advance();
            if (match('=')) { addToken(TokenType::NEQ, "!=", startLine, startCol); return; }
            addToken(TokenType::BANG, "!", startLine, startCol);
            return;

        case '<':
            advance();
            if (match('=')) { addToken(TokenType::LE, "<=", startLine, startCol); return; }
            if (match('<')) { addToken(TokenType::DOUBLE_LT, "<<", startLine, startCol); return; }
            addToken(TokenType::LT, "<", startLine, startCol);
            return;

        case '>':
            advance();
            if (match('=')) { addToken(TokenType::GE, ">=", startLine, startCol); return; }
            if (match('>')) { addToken(TokenType::DOUBLE_GT, ">>", startLine, startCol); return; }
            addToken(TokenType::GT, ">", startLine, startCol);
            return;

        case '+':
            advance();
            if (match('+')) { addToken(TokenType::INCREMENT, "++", startLine, startCol); return; }
            if (match('=')) { addToken(TokenType::PLUS_ASSIGN, "+=", startLine, startCol); return; }
            addToken(TokenType::PLUS, "+", startLine, startCol);
            return;

        case '-':
            advance();
            if (match('-')) { addToken(TokenType::DECREMENT, "--", startLine, startCol); return; }
            if (match('=')) { addToken(TokenType::MINUS_ASSIGN, "-=", startLine, startCol); return; }
            addToken(TokenType::MINUS, "-", startLine, startCol);
            return;

        case '*':
            advance();
            if (match('=')) { addToken(TokenType::STAR_ASSIGN, "*=", startLine, startCol); return; }
            addToken(TokenType::STAR, "*", startLine, startCol);
            return;

        case '/':
            advance();
            if (match('=')) { addToken(TokenType::SLASH_ASSIGN, "/=", startLine, startCol); return; }
            addToken(TokenType::SLASH, "/", startLine, startCol);
            return;

        case '%': advance(); addToken(TokenType::PERCENT, "%", startLine, startCol); return;
        case '^': advance(); addToken(TokenType::CARET, "^", startLine, startCol); return;

        case '&':
            advance();
            if (match('&')) { addToken(TokenType::AND_AND, "&&", startLine, startCol); return; }
            addToken(TokenType::AMP, "&", startLine, startCol);
            return;

        case '|':
            advance();
            if (match('|')) { addToken(TokenType::OR_OR, "||", startLine, startCol); return; }
            if (match('>')) { addToken(TokenType::PIPE, "|>", startLine, startCol); return; }
            addToken(TokenType::PIPE_BIT, "|", startLine, startCol);
            return;

        case '?':
            advance();
            if (match('.')) { addToken(TokenType::SAFE_NAV, "?.", startLine, startCol); return; }
            if (match('?')) { addToken(TokenType::NULL_COALESCE, "??", startLine, startCol); return; }
            if (match(':')) { addToken(TokenType::ELVIS, "?:", startLine, startCol); return; }
            addToken(TokenType::QUESTION, "?", startLine, startCol);
            return;

        default:
            throw LexError(std::string("unexpected character '") + c + "'", startLine, startCol);
    }
}

std::vector<Token> Lexer::tokenize() {
    // Process indentation for the very first line too.
    handleLeadingWhitespaceAndIndent();

    while (!isAtEnd()) {
        scanToken();
    }

    // Final logical-line newline + unwind any remaining indentation.
    emitNewlineIfNeeded();
    while (indentStack.size() > 1) {
        indentStack.pop_back();
        addToken(TokenType::DEDENT, "<dedent>", line, col);
    }

    addToken(TokenType::END_OF_FILE, "<eof>", line, col);
    return tokens;
}

} // namespace nova
