// Token.hpp — Nova Language Token Definitions
// Covers Grammar 1 (EBNF), Grammar 2 (Keyword Dictionary), Grammar 3 (Operational Grammar)
// and all 25 Super-Syntax Extensions.
#pragma once
#include <string>
#include <unordered_map>
#include <cstdint>

namespace nova {

enum class TokenType {
    // ---- Literals ----
    IDENTIFIER, INT_LITERAL, FLOAT_LITERAL, STRING_LITERAL, RAW_STRING_LITERAL,
    TRUE_LIT, FALSE_LIT, NULL_LIT,

    // ---- Structural ----
    NEWLINE, INDENT, DEDENT, END_OF_FILE,

    // ---- Punctuation ----
    LPAREN, RPAREN,          // ( )
    LBRACE, RBRACE,          // { }
    LBRACKET, RBRACKET,      // [ ]
    COMMA, DOT, COLON, SEMICOLON, ARROW_FAT, // , . : ; =>
    AT,                      // @  (annotations)
    HASH,                    // #  (single-line comment alt / used already stripped by lexer)
    DOLLAR_PARAM,            // $0 $1 lambda shorthand params

    // ---- Assignment / declaration ----
    ASSIGN,                  // =
    PLUS_ASSIGN, MINUS_ASSIGN, STAR_ASSIGN, SLASH_ASSIGN, // += -= *= /=
    INCREMENT, DECREMENT,    // ++ --

    // ---- Arithmetic ----
    PLUS, MINUS, STAR, SLASH, PERCENT, CARET, // + - * / % ^

    // ---- Relational / Equality ----
    EQ, NEQ, LT, GT, LE, GE, // == != < > <= >=

    // ---- Logical ----
    AND_AND, OR_OR, BANG,    // && || !

    // ---- Nova special operators (25 super-syntax extensions) ----
    PIPE,                    // |>            (2. pipe operator)
    ELVIS,                   // ?:            (3. elvis operator)
    QUESTION,                // ?             (ternary)
    SAFE_NAV,                // ?.            (23. safe navigation)
    NULL_COALESCE,           // ??            (extension: null coalescing)
    BANG_PROPAGATE,          // !  postfix on call, reuse BANG contextually (6. error propagation `result!`)
    RANGE,                   // ..            (5. smart range a..b)
    ELLIPSIS,                // ...           (20. spread/rest)
    SLICE_COLON,             // : inside [ ]  (1. slicing) — reuses COLON, disambiguated by parser
    DOUBLE_LT, DOUBLE_GT,    // << >> (bitwise, used in masking/bitfield contexts if needed)
    AMP, PIPE_BIT, TILDE,    // & | ~  bitwise ops (support for Ops/bit masking, extension 15)

    // ---- Keywords: Declaration ----
    KW_CONST, KW_LET, KW_VAR, KW_MUT,

    // ---- Keywords: Control flow ----
    KW_IF, KW_ELSE, KW_ELIF, KW_SWITCH, KW_CASE, KW_DEFAULT, KW_MATCH,

    // ---- Keywords: Loops ----
    KW_FOR, KW_TO, KW_STEP, KW_WHILE, KW_REPEAT, KW_FOREACH, KW_IN,
    KW_BREAK, KW_CONTINUE,

    // ---- Keywords: Functions & OOP ----
    KW_FUNCTION, KW_FN, KW_RETURN, KW_CLASS, KW_STRUCT, KW_INTERFACE,
    KW_ENUM, KW_SELF, KW_EXTEND,

    // ---- Keywords: modules/runtime namespaces ----
    KW_MODULE, KW_IMPORT, KW_EXPORT,
    KW_NOVA_NS, KW_OPS, KW_OSN, KW_OKL, KW_ASTER_LOWER, KW_ASTER_UPPER, KW_ESTER,

    // ---- Keywords: concurrency / error handling ----
    KW_TRY, KW_CATCH, KW_FINALLY, KW_THREAD, KW_ASYNC, KW_AWAIT,
    KW_UNSAFE, KW_ON,

    // ---- Keywords: extension-syntax (25 super-syntax) ----
    KW_SIGNAL,      // 8. reactivity signals
    KW_USING,       // 13. automatic resource management
    KW_YIELD,       // 12. coroutine generators
    KW_CHAN,        // 14. concurrency channels chan<T>
    KW_GUARD,       // 16. guard clauses
    KW_TYPE,        // 21. type aliasing
    KW_LAZY,        // 24. lazy evaluation
    KW_COMPTIME,    // 25. compile-time assertions
    KW_MACRO,       // macro definitions
    KW_AS_SAFE,     // as?  (11. safe pointer casting) — 'as' keyword + '?' token combo
    KW_AS,          // as   (cast keyword)
    KW_EXISTS,      // "if value exists:" existence assertion

    // ---- Keywords: collection type annotations ----
    KW_LIST, KW_SET, KW_TUPLE, KW_MAP,

    UNKNOWN
};

// Human readable name (for diagnostics / AST dump)
inline const char* tokenTypeName(TokenType t) {
    switch (t) {
        case TokenType::IDENTIFIER: return "IDENTIFIER";
        case TokenType::INT_LITERAL: return "INT_LITERAL";
        case TokenType::FLOAT_LITERAL: return "FLOAT_LITERAL";
        case TokenType::STRING_LITERAL: return "STRING_LITERAL";
        case TokenType::RAW_STRING_LITERAL: return "RAW_STRING_LITERAL";
        case TokenType::TRUE_LIT: return "TRUE";
        case TokenType::FALSE_LIT: return "FALSE";
        case TokenType::NULL_LIT: return "NULL";
        case TokenType::NEWLINE: return "NEWLINE";
        case TokenType::INDENT: return "INDENT";
        case TokenType::DEDENT: return "DEDENT";
        case TokenType::END_OF_FILE: return "EOF";
        case TokenType::LPAREN: return "LPAREN";
        case TokenType::RPAREN: return "RPAREN";
        case TokenType::LBRACE: return "LBRACE";
        case TokenType::RBRACE: return "RBRACE";
        case TokenType::LBRACKET: return "LBRACKET";
        case TokenType::RBRACKET: return "RBRACKET";
        case TokenType::COMMA: return "COMMA";
        case TokenType::DOT: return "DOT";
        case TokenType::COLON: return "COLON";
        case TokenType::SEMICOLON: return "SEMICOLON";
        case TokenType::ARROW_FAT: return "ARROW_FAT";
        case TokenType::AT: return "AT";
        case TokenType::HASH: return "HASH";
        case TokenType::DOLLAR_PARAM: return "DOLLAR_PARAM";
        case TokenType::ASSIGN: return "ASSIGN";
        case TokenType::PLUS_ASSIGN: return "PLUS_ASSIGN";
        case TokenType::MINUS_ASSIGN: return "MINUS_ASSIGN";
        case TokenType::STAR_ASSIGN: return "STAR_ASSIGN";
        case TokenType::SLASH_ASSIGN: return "SLASH_ASSIGN";
        case TokenType::INCREMENT: return "INCREMENT";
        case TokenType::DECREMENT: return "DECREMENT";
        case TokenType::PLUS: return "PLUS";
        case TokenType::MINUS: return "MINUS";
        case TokenType::STAR: return "STAR";
        case TokenType::SLASH: return "SLASH";
        case TokenType::PERCENT: return "PERCENT";
        case TokenType::CARET: return "CARET";
        case TokenType::EQ: return "EQ";
        case TokenType::NEQ: return "NEQ";
        case TokenType::LT: return "LT";
        case TokenType::GT: return "GT";
        case TokenType::LE: return "LE";
        case TokenType::GE: return "GE";
        case TokenType::AND_AND: return "AND_AND";
        case TokenType::OR_OR: return "OR_OR";
        case TokenType::BANG: return "BANG";
        case TokenType::PIPE: return "PIPE";
        case TokenType::ELVIS: return "ELVIS";
        case TokenType::QUESTION: return "QUESTION";
        case TokenType::SAFE_NAV: return "SAFE_NAV";
        case TokenType::NULL_COALESCE: return "NULL_COALESCE";
        case TokenType::RANGE: return "RANGE";
        case TokenType::ELLIPSIS: return "ELLIPSIS";
        case TokenType::DOUBLE_LT: return "DOUBLE_LT";
        case TokenType::DOUBLE_GT: return "DOUBLE_GT";
        case TokenType::AMP: return "AMP";
        case TokenType::PIPE_BIT: return "PIPE_BIT";
        case TokenType::TILDE: return "TILDE";
        default: return "KEYWORD_OR_OTHER";
    }
}

struct Token {
    TokenType type;
    std::string lexeme;
    int line;
    int column;
    // literal payloads
    int64_t intValue = 0;
    double floatValue = 0.0;

    Token() : type(TokenType::UNKNOWN), lexeme(""), line(0), column(0) {}
    Token(TokenType t, std::string lex, int ln, int col)
        : type(t), lexeme(std::move(lex)), line(ln), column(col) {}
};

// Keyword table — maps raw identifier text to its TokenType.
// This is the single source of truth for Grammar 2 keyword dictionary.
inline const std::unordered_map<std::string, TokenType>& keywordTable() {
    static const std::unordered_map<std::string, TokenType> table = {
        {"const", TokenType::KW_CONST}, {"let", TokenType::KW_LET},
        {"var", TokenType::KW_VAR},     {"mut", TokenType::KW_MUT},

        {"if", TokenType::KW_IF}, {"else", TokenType::KW_ELSE}, {"elif", TokenType::KW_ELIF},
        {"switch", TokenType::KW_SWITCH}, {"case", TokenType::KW_CASE},
        {"default", TokenType::KW_DEFAULT}, {"match", TokenType::KW_MATCH},

        {"for", TokenType::KW_FOR}, {"to", TokenType::KW_TO}, {"step", TokenType::KW_STEP},
        {"while", TokenType::KW_WHILE}, {"repeat", TokenType::KW_REPEAT},
        {"foreach", TokenType::KW_FOREACH}, {"in", TokenType::KW_IN},
        {"break", TokenType::KW_BREAK}, {"continue", TokenType::KW_CONTINUE},

        {"function", TokenType::KW_FUNCTION}, {"fn", TokenType::KW_FN},
        {"return", TokenType::KW_RETURN}, {"class", TokenType::KW_CLASS},
        {"struct", TokenType::KW_STRUCT}, {"interface", TokenType::KW_INTERFACE},
        {"enum", TokenType::KW_ENUM}, {"self", TokenType::KW_SELF},
        {"extend", TokenType::KW_EXTEND},

        {"module", TokenType::KW_MODULE}, {"import", TokenType::KW_IMPORT},
        {"export", TokenType::KW_EXPORT},
        {"Nova", TokenType::KW_NOVA_NS}, {"Ops", TokenType::KW_OPS},
        {"osn", TokenType::KW_OSN}, {"okl", TokenType::KW_OKL},
        {"aster", TokenType::KW_ASTER_LOWER}, {"Aster", TokenType::KW_ASTER_UPPER},
        {"ester", TokenType::KW_ESTER},

        {"try", TokenType::KW_TRY}, {"catch", TokenType::KW_CATCH},
        {"finally", TokenType::KW_FINALLY}, {"thread", TokenType::KW_THREAD},
        {"async", TokenType::KW_ASYNC}, {"await", TokenType::KW_AWAIT},
        {"unsafe", TokenType::KW_UNSAFE}, {"on", TokenType::KW_ON},

        {"signal", TokenType::KW_SIGNAL}, {"using", TokenType::KW_USING},
        {"yield", TokenType::KW_YIELD}, {"chan", TokenType::KW_CHAN},
        {"guard", TokenType::KW_GUARD}, {"type", TokenType::KW_TYPE},
        {"lazy", TokenType::KW_LAZY}, {"comptime", TokenType::KW_COMPTIME},
        {"macro", TokenType::KW_MACRO}, {"as", TokenType::KW_AS},
        {"exists", TokenType::KW_EXISTS},

        {"List", TokenType::KW_LIST}, {"Set", TokenType::KW_SET},
        {"Tuple", TokenType::KW_TUPLE}, {"Map", TokenType::KW_MAP},

        {"true", TokenType::TRUE_LIT}, {"false", TokenType::FALSE_LIT},
        {"null", TokenType::NULL_LIT}, {"nil", TokenType::NULL_LIT},
    };
    return table;
}

} // namespace nova
