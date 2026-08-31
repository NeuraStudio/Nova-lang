// Parser.hpp — Nova Language recursive-descent Parser.
// Implements every production in Grammar 1 (EBNF) with correct precedence
// climbing for expressions and full dual brace/indentation block support.
#pragma once
#include "Token.hpp"
#include "AST.hpp"
#include <vector>
#include <memory>
#include <stdexcept>

namespace nova {

class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& msg, int line, int col)
        : std::runtime_error("Parse error [line " + std::to_string(line) +
                              ", col " + std::to_string(col) + "]: " + msg),
          line(line), column(col) {}
    int line, column;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    std::unique_ptr<Program> parseProgram();

private:
    std::vector<Token> toks;
    size_t pos = 0;

    // ---- cursor helpers ----
    const Token& peek(int offset = 0) const;
    const Token& previous() const;
    bool isAtEnd() const;
    const Token& advance();
    bool check(TokenType t) const;
    bool match(std::initializer_list<TokenType> types);
    const Token& expect(TokenType type, const std::string& message);
    void skipNewlines();      // consume any run of NEWLINE tokens
    void skipStatementEnd();  // consume NEWLINE/SEMICOLON separators
    // Accepts a plain IDENTIFIER or any of the reserved namespace/collection
    // keywords (Nova, Ops, osn, okl, aster, Aster, ester, self, List, Set,
    // Tuple, Map) wherever a name is expected in a qualified path.
    Token expectIdentifierLike(const std::string& message);

    // ---- blocks (Grammar 1: block = brace_block | indent_block) ----
    BlockPtr parseBlock();
    BlockPtr parseBraceBlock();
    BlockPtr parseIndentBlock();

    // ---- statements ----
    StmtPtr parseStatement();
    StmtPtr parseDeclarationOrAssignmentOrExpr();
    StmtPtr parseIfStmt();
    StmtPtr parseSwitchStmt();
    StmtPtr parseForStmt();
    StmtPtr parseWhileStmt();
    StmtPtr parseRepeatStmt();
    StmtPtr parseForeachStmt();
    StmtPtr parseFunctionDecl();
    StmtPtr parseClassDecl();
    StmtPtr parseInterfaceDecl();
    StmtPtr parseEnumDecl();
    StmtPtr parseStructDecl();
    StmtPtr parseModuleDecl();
    StmtPtr parseImportStmt();
    StmtPtr parseExportStmt();
    StmtPtr parseTryStmt();
    StmtPtr parseThreadStmt();
    StmtPtr parseAsyncStmt();
    StmtPtr parseEventStmt();
    StmtPtr parseUnsafeStmt();
    StmtPtr parseSignalDecl();
    StmtPtr parseUsingStmt();
    StmtPtr parseGuardStmt();
    StmtPtr parseTypeAliasStmt();
    StmtPtr parseExtendStmt();
    StmtPtr parseLazyDecl();
    StmtPtr parseComptimeStmt();
    StmtPtr parseMacroDecl();
    StmtPtr parseChanDecl();
    StmtPtr parseAnnotatedStmt();
    StmtPtr parseCollectionTypeDecl(); // List<T> x / Set x / Tuple x=(...) / Map<K,V> x

    // ---- expressions (precedence climbing per Grammar 1) ----
    ExprPtr parseExpression();
    ExprPtr parseAssignmentExpr();
    ExprPtr parseTernaryExpr();
    ExprPtr parseElvisOrCoalesceExpr();
    ExprPtr parsePipeExpr();
    ExprPtr parseLogicalOrExpr();
    ExprPtr parseLogicalAndExpr();
    ExprPtr parseEqualityExpr();
    ExprPtr parseRelationalExpr();
    ExprPtr parseRangeExpr();
    ExprPtr parseAdditiveExpr();
    ExprPtr parseMultiplicativeExpr();
    ExprPtr parseUnaryExpr();
    ExprPtr parsePostfixExpr();
    ExprPtr parsePrimaryExpr();
    ExprPtr parseLambdaExpr();
    ExprPtr parseMatchExpr();
    ExprPtr parseArrayOrSliceLiteral();
    ExprPtr parseMapLiteral();
    std::vector<ExprPtr> parseArgs();

    TypeRef parseTypeRef();
    bool looksLikeLambdaStart();
};

} // namespace nova
