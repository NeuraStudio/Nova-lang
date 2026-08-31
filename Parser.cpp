// Parser.cpp — Nova Language recursive-descent Parser implementation.
#include "Parser.hpp"

namespace nova {

Parser::Parser(std::vector<Token> tokens) : toks(std::move(tokens)) {}

// ═══════════════════════════ cursor helpers ═══════════════════════════

const Token& Parser::peek(int offset) const {
    size_t p = pos + offset;
    if (p >= toks.size()) return toks.back(); // EOF
    return toks[p];
}
const Token& Parser::previous() const { return toks[pos - 1]; }
bool Parser::isAtEnd() const { return peek().type == TokenType::END_OF_FILE; }

const Token& Parser::advance() {
    if (!isAtEnd()) pos++;
    return previous();
}

bool Parser::check(TokenType t) const {
    if (isAtEnd() && t != TokenType::END_OF_FILE) return false;
    return peek().type == t;
}

bool Parser::match(std::initializer_list<TokenType> types) {
    for (auto t : types) {
        if (check(t)) { advance(); return true; }
    }
    return false;
}

const Token& Parser::expect(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    throw ParseError(message + " (got '" + peek().lexeme + "' / " +
                      tokenTypeName(peek().type) + ")", peek().line, peek().column);
}

void Parser::skipNewlines() {
    while (check(TokenType::NEWLINE)) advance();
}

void Parser::skipStatementEnd() {
    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON)) advance();
}

Token Parser::expectIdentifierLike(const std::string& message) {
    switch (peek().type) {
        case TokenType::IDENTIFIER:
        case TokenType::KW_NOVA_NS: case TokenType::KW_OPS: case TokenType::KW_OSN:
        case TokenType::KW_OKL: case TokenType::KW_ASTER_LOWER: case TokenType::KW_ASTER_UPPER:
        case TokenType::KW_ESTER: case TokenType::KW_SELF:
        case TokenType::KW_LIST: case TokenType::KW_SET: case TokenType::KW_TUPLE: case TokenType::KW_MAP:
            return advance();
        default:
            throw ParseError(message + " (got '" + peek().lexeme + "' / " +
                              tokenTypeName(peek().type) + ")", peek().line, peek().column);
    }
}

// ═══════════════════════════ blocks ═══════════════════════════
// Grammar 1: block = brace_block | indent_block
//   brace_block  = "{" { statement } "}"
//   indent_block = ":" NEWLINE INDENT { statement } DEDENT
// Both resolve to the same Block AST node (Grammar 3-A).

BlockPtr Parser::parseBlock() {
    skipNewlines();
    if (check(TokenType::LBRACE)) return parseBraceBlock();
    if (check(TokenType::COLON)) return parseIndentBlock();
    throw ParseError("expected block ('{' or ':') ", peek().line, peek().column);
}

BlockPtr Parser::parseBraceBlock() {
    auto block = std::make_unique<Block>();
    block->line = peek().line; block->col = peek().column;
    expect(TokenType::LBRACE, "expected '{'");
    skipNewlines();
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        block->statements.push_back(parseStatement());
        skipStatementEnd();
    }
    expect(TokenType::RBRACE, "expected '}' to close block");
    return block;
}

BlockPtr Parser::parseIndentBlock() {
    auto block = std::make_unique<Block>();
    block->line = peek().line; block->col = peek().column;
    expect(TokenType::COLON, "expected ':' to start indented block");
    skipNewlines();
    // A single-statement body on the same line is also tolerated, e.g.
    //   default: Nova.show("Unknown")
    if (!check(TokenType::INDENT)) {
        block->statements.push_back(parseStatement());
        return block;
    }
    expect(TokenType::INDENT, "expected indentation to open block");
    skipNewlines();
    while (!check(TokenType::DEDENT) && !isAtEnd()) {
        block->statements.push_back(parseStatement());
        skipStatementEnd();
    }
    if (check(TokenType::DEDENT)) advance();
    return block;
}

// ═══════════════════════════ program entry ═══════════════════════════

std::unique_ptr<Program> Parser::parseProgram() {
    auto program = std::make_unique<Program>();
    skipStatementEnd();
    while (!isAtEnd()) {
        program->statements.push_back(parseStatement());
        skipStatementEnd();
    }
    return program;
}

// ═══════════════════════════ statement dispatch ═══════════════════════════
// Grammar 1: statement = declaration | assignment | expr_stmt | if_stmt | switch_stmt
//          | for_stmt | while_stmt | repeat_stmt | foreach_stmt
//          | function_decl | class_decl | interface_decl | enum_decl
//          | struct_decl | module_decl | import_stmt | export_stmt
//          | try_stmt | thread_stmt | async_stmt | event_stmt | unsafe_stmt ;
// Plus the 25 super-syntax extension statement forms.

StmtPtr Parser::parseStatement() {
    skipNewlines();

    if (check(TokenType::AT)) return parseAnnotatedStmt();

    switch (peek().type) {
        case TokenType::KW_IF:        return parseIfStmt();
        case TokenType::KW_SWITCH:    return parseSwitchStmt();
        case TokenType::KW_FOR:       return parseForStmt();
        case TokenType::KW_WHILE:     return parseWhileStmt();
        case TokenType::KW_REPEAT:    return parseRepeatStmt();
        case TokenType::KW_FOREACH:   return parseForeachStmt();
        case TokenType::KW_FUNCTION:
        case TokenType::KW_FN:        return parseFunctionDecl();
        case TokenType::KW_CLASS:     return parseClassDecl();
        case TokenType::KW_INTERFACE: return parseInterfaceDecl();
        case TokenType::KW_ENUM:      return parseEnumDecl();
        case TokenType::KW_STRUCT:    return parseStructDecl();
        case TokenType::KW_MODULE:    return parseModuleDecl();
        case TokenType::KW_IMPORT:    return parseImportStmt();
        case TokenType::KW_EXPORT:    return parseExportStmt();
        case TokenType::KW_TRY:       return parseTryStmt();
        case TokenType::KW_THREAD:    return parseThreadStmt();
        case TokenType::KW_ASYNC:     return parseAsyncStmt();
        case TokenType::KW_ON:        return parseEventStmt();
        case TokenType::KW_UNSAFE:    return parseUnsafeStmt();
        case TokenType::KW_SIGNAL:    return parseSignalDecl();
        case TokenType::KW_USING:     return parseUsingStmt();
        case TokenType::KW_GUARD:     return parseGuardStmt();
        case TokenType::KW_TYPE:      return parseTypeAliasStmt();
        case TokenType::KW_EXTEND:    return parseExtendStmt();
        case TokenType::KW_LAZY:      return parseLazyDecl();
        case TokenType::KW_COMPTIME:  return parseComptimeStmt();
        case TokenType::KW_MACRO:     return parseMacroDecl();
        case TokenType::KW_CHAN:      return parseChanDecl();
        case TokenType::KW_LIST:
        case TokenType::KW_SET:
        case TokenType::KW_TUPLE:
        case TokenType::KW_MAP:       return parseCollectionTypeDecl();

        case TokenType::KW_BREAK: {
            auto s = std::make_unique<BreakStmt>();
            s->line = peek().line; advance(); return s;
        }
        case TokenType::KW_CONTINUE: {
            auto s = std::make_unique<ContinueStmt>();
            s->line = peek().line; advance(); return s;
        }
        case TokenType::KW_RETURN: {
            auto s = std::make_unique<ReturnStmt>();
            s->line = peek().line; advance();
            if (!check(TokenType::NEWLINE) && !check(TokenType::SEMICOLON) &&
                !check(TokenType::RBRACE) && !check(TokenType::DEDENT) && !isAtEnd()) {
                s->value = parseExpression();
            }
            return s;
        }
        case TokenType::KW_YIELD: {
            auto s = std::make_unique<YieldStmt>();
            s->line = peek().line; advance();
            s->value = parseExpression();
            return s;
        }

        default:
            return parseDeclarationOrAssignmentOrExpr();
    }
}

// Annotations: @Public, @Optimize("O3") ... attached to the following statement.
StmtPtr Parser::parseAnnotatedStmt() {
    auto node = std::make_unique<AnnotatedStmt>();
    node->line = peek().line;
    while (check(TokenType::AT)) {
        advance();
        Annotation ann;
        ann.name = expect(TokenType::IDENTIFIER, "expected annotation name after '@'").lexeme;
        if (check(TokenType::LPAREN)) {
            advance();
            if (!check(TokenType::RPAREN)) {
                ann.args.push_back(parseExpression());
                while (match({TokenType::COMMA})) ann.args.push_back(parseExpression());
            }
            expect(TokenType::RPAREN, "expected ')' after annotation arguments");
        }
        node->annotations.push_back(std::move(ann));
        skipNewlines();
    }
    node->inner = parseStatement();
    return node;
}

// ═══════════════════════════ declaration / assignment / expr_stmt ═══════════════════════════
// Grammar 1:
//   declaration = [ "const" ] identifier "=" expression ;
//   assignment  = lvalue "=" expression ;
//   lvalue      = identifier { "." identifier | "[" expression "]" } ;
// Extended to also support: mut, tuple destructuring `a, b = b, a`,
// array destructuring `let [x, y] = point`, compound assignment (+= -= *= /=),
// and bare typed declarations like `List users`.

StmtPtr Parser::parseDeclarationOrAssignmentOrExpr() {
    int startLine = peek().line, startCol = peek().column;

    bool isConst = false, isMut = false, hasLetVar = false;
    if (check(TokenType::KW_CONST)) { isConst = true; advance(); }
    else if (check(TokenType::KW_LET) || check(TokenType::KW_VAR)) { hasLetVar = true; advance(); }
    if (check(TokenType::KW_MUT)) { isMut = true; advance(); }

    // Destructuring: let [x, y] = point   OR  [x, y] = point
    if (check(TokenType::LBRACKET)) {
        advance();
        auto d = std::make_unique<DestructuringStmt>();
        d->line = startLine; d->col = startCol; d->isArrayPattern = true;
        d->targets.push_back(expect(TokenType::IDENTIFIER, "expected identifier in destructuring pattern").lexeme);
        while (match({TokenType::COMMA})) {
            d->targets.push_back(expect(TokenType::IDENTIFIER, "expected identifier in destructuring pattern").lexeme);
        }
        expect(TokenType::RBRACKET, "expected ']' to close destructuring pattern");
        expect(TokenType::ASSIGN, "expected '=' in destructuring assignment");
        d->value = parseExpression();
        return d;
    }

    // Everything else starts with an identifier (or a full lvalue/expression).
    // Try to detect tuple-swap / multi-target destructuring: `a, b = b, a`
    if (check(TokenType::IDENTIFIER)) {
        size_t save = pos;
        std::string firstName = peek().lexeme;
        advance();
        if (check(TokenType::COMMA)) {
            // Lookahead: identifier { , identifier } = expr { , expr }
            std::vector<std::string> names{firstName};
            size_t lookahead = pos;
            bool isDestructure = true;
            while (toks[lookahead].type == TokenType::COMMA) {
                lookahead++;
                if (toks[lookahead].type != TokenType::IDENTIFIER) { isDestructure = false; break; }
                names.push_back(toks[lookahead].lexeme);
                lookahead++;
            }
            if (isDestructure && toks[lookahead].type == TokenType::ASSIGN) {
                pos = lookahead;
                advance(); // consume '='
                auto d = std::make_unique<DestructuringStmt>();
                d->line = startLine; d->col = startCol; d->isArrayPattern = false;
                d->targets = names;
                // Parse RHS as a comma-separated expression list packed into a tuple literal.
                auto tup = std::make_unique<TupleLiteralExpr>();
                tup->elements.push_back(parseExpression());
                while (match({TokenType::COMMA})) tup->elements.push_back(parseExpression());
                if (tup->elements.size() == 1) d->value = std::move(tup->elements[0]);
                else d->value = std::move(tup);
                return d;
            }
        }
        pos = save; // rewind, not a destructure
    }

    // Bare typed collection-ish declaration handled separately (parseCollectionTypeDecl),
    // so here: parse a full expression/lvalue, then decide if it's a declaration/assignment.
    ExprPtr lhs = parseExpression();

    // const/let/var NAME = expr  (declaration form, lhs collapses to identifier)
    if (isConst || hasLetVar) {
        auto decl = std::make_unique<DeclarationStmt>();
        decl->line = startLine; decl->col = startCol;
        decl->isConst = isConst; decl->isMut = isMut;
        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            decl->name = id->name;
        } else {
            throw ParseError("expected identifier after declaration keyword", startLine, startCol);
        }
        if (match({TokenType::ASSIGN})) {
            decl->value = parseExpression();
        }
        return decl;
    }

    // Compound / plain assignment: lvalue (= | += | -= | *= | /=) expr
    if (check(TokenType::ASSIGN) || check(TokenType::PLUS_ASSIGN) || check(TokenType::MINUS_ASSIGN) ||
        check(TokenType::STAR_ASSIGN) || check(TokenType::SLASH_ASSIGN)) {
        std::string op = advance().lexeme;
        auto asg = std::make_unique<AssignmentStmt>();
        asg->line = startLine; asg->col = startCol;
        asg->target = std::move(lhs);
        asg->op = op;
        asg->value = parseExpression();
        return asg;
    }

    // Bare identifier assignment shorthand with no keyword: `name = value`
    // already covered above since parseExpression parses just the identifier
    // and then we see ASSIGN. If not, this is a plain expression statement
    // (e.g. a bare function call `Nova.show(...)`, or `health--`/`health++`).
    auto stmt = std::make_unique<ExprStmt>();
    stmt->line = startLine; stmt->col = startCol;
    stmt->expr = std::move(lhs);
    return stmt;
}

// Bare typed declarations: `List users`, `Set fruits`, `Tuple location=(20,40)`,
// `List<String> users`, `Map<String,User> database`.
StmtPtr Parser::parseCollectionTypeDecl() {
    int startLine = peek().line, startCol = peek().column;
    TypeRef type = parseTypeRef();
    std::string name = expect(TokenType::IDENTIFIER, "expected variable name after collection type").lexeme;

    auto decl = std::make_unique<DeclarationStmt>();
    decl->line = startLine; decl->col = startCol;
    decl->name = name;
    decl->typeAnnotation = type;
    if (match({TokenType::ASSIGN})) {
        decl->value = parseExpression();
    }
    return decl;
}

TypeRef Parser::parseTypeRef() {
    TypeRef t;
    t.name = advance().lexeme; // consumes List/Set/Tuple/Map/identifier keyword token
    if (check(TokenType::LT)) {
        advance();
        t.generics.push_back(parseTypeRef());
        while (match({TokenType::COMMA})) t.generics.push_back(parseTypeRef());
        expect(TokenType::GT, "expected '>' to close generic type parameter list");
    }
    return t;
}

// ═══════════════════════════ control flow ═══════════════════════════
// if_stmt = "if" expression block [ "else" block ] ;
// Also supports "elif" chains and Python-style "else if".
StmtPtr Parser::parseIfStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_IF, "expected 'if'");
    auto node = std::make_unique<IfStmt>();
    node->line = ln; node->col = cl;
    node->condition = parseExpression();
    node->thenBranch = parseBlock();
    skipNewlines();
    if (check(TokenType::KW_ELIF)) {
        auto elif = parseIfStmt(); // 'elif' reuses if-parsing logic below
        node->elseIf.reset(static_cast<IfStmt*>(elif.release()));
    } else if (check(TokenType::KW_ELSE)) {
        advance();
        skipNewlines();
        if (check(TokenType::KW_IF)) {
            auto elseIfBranch = parseIfStmt();
            node->elseIf.reset(static_cast<IfStmt*>(elseIfBranch.release()));
        } else {
            node->elseBranch = parseBlock();
        }
    }
    return node;
}

// switch_stmt = "switch" "(" expression ")" ( "{" case* default? "}" | ":" NEWLINE INDENT case* default? DEDENT )
// case_clause = "case" expression ":" { statement } [ "break" ] ;
StmtPtr Parser::parseSwitchStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_SWITCH, "expected 'switch'");
    expect(TokenType::LPAREN, "expected '(' after 'switch'");
    auto node = std::make_unique<SwitchStmt>();
    node->line = ln; node->col = cl;
    node->subject = parseExpression();
    expect(TokenType::RPAREN, "expected ')' after switch subject");
    skipNewlines();

    bool brace = check(TokenType::LBRACE);
    if (brace) { advance(); } else { expect(TokenType::COLON, "expected ':' or '{' to open switch body"); }
    skipNewlines();
    if (!brace && check(TokenType::INDENT)) advance();
    skipNewlines();

    auto atEnd = [&]() {
        if (brace) return check(TokenType::RBRACE);
        return check(TokenType::DEDENT) || isAtEnd();
    };

    while (!atEnd()) {
        if (check(TokenType::KW_CASE)) {
            advance();
            CaseClause cc;
            cc.value = parseExpression();
            expect(TokenType::COLON, "expected ':' after case value");
            skipNewlines();
            while (!check(TokenType::KW_CASE) && !check(TokenType::KW_DEFAULT) && !atEnd()) {
                if (check(TokenType::KW_BREAK)) { advance(); cc.hasBreak = true; skipStatementEnd(); continue; }
                cc.body.push_back(parseStatement());
                skipStatementEnd();
            }
            node->cases.push_back(std::move(cc));
        } else if (check(TokenType::KW_DEFAULT)) {
            advance();
            expect(TokenType::COLON, "expected ':' after 'default'");
            skipNewlines();
            node->hasDefault = true;
            while (!atEnd()) {
                node->defaultBody.push_back(parseStatement());
                skipStatementEnd();
            }
        } else {
            skipNewlines();
            if (atEnd()) break;
            throw ParseError("expected 'case' or 'default' in switch body", peek().line, peek().column);
        }
    }
    if (brace) expect(TokenType::RBRACE, "expected '}' to close switch");
    else if (check(TokenType::DEDENT)) advance();
    return node;
}

// for_stmt = "for" identifier "=" expression "to" expression block ;
// Extended with optional "step" clause. Also tolerates the range-style form
// `for i in 1..100 step 5:` (Extension #5), which is parsed as a Foreach
// over a Range expression for uniformity with the foreach_stmt production.
StmtPtr Parser::parseForStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_FOR, "expected 'for'");
    std::string varName = expect(TokenType::IDENTIFIER, "expected loop variable name").lexeme;

    if (match({TokenType::KW_IN})) {
        auto node = std::make_unique<ForeachStmt>();
        node->line = ln; node->col = cl;
        node->varName = varName;
        node->iterable = parseExpression(); // e.g. Range expr: 1..100 step 5
        node->body = parseBlock();
        return node;
    }

    auto node = std::make_unique<ForStmt>();
    node->line = ln; node->col = cl;
    node->varName = varName;
    expect(TokenType::ASSIGN, "expected '=' in for-loop header");
    node->from = parseExpression();
    expect(TokenType::KW_TO, "expected 'to' in for-loop header");
    node->to = parseExpression();
    if (match({TokenType::KW_STEP})) node->step = parseExpression();
    node->body = parseBlock();
    return node;
}

StmtPtr Parser::parseWhileStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_WHILE, "expected 'while'");
    auto node = std::make_unique<WhileStmt>();
    node->line = ln; node->col = cl;
    node->condition = parseExpression();
    node->body = parseBlock();
    return node;
}

StmtPtr Parser::parseRepeatStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_REPEAT, "expected 'repeat'");
    auto node = std::make_unique<RepeatStmt>();
    node->line = ln; node->col = cl;
    node->count = parseExpression();
    node->body = parseBlock();
    return node;
}

StmtPtr Parser::parseForeachStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_FOREACH, "expected 'foreach'");
    auto node = std::make_unique<ForeachStmt>();
    node->line = ln; node->col = cl;
    node->varName = expect(TokenType::IDENTIFIER, "expected loop variable name").lexeme;
    expect(TokenType::KW_IN, "expected 'in' in foreach header");
    node->iterable = parseExpression();
    node->body = parseBlock();
    return node;
}

// ═══════════════════════════ functions / OOP ═══════════════════════════
// function_decl = ( "function" | "fn" ) identifier [ generic_params ] "(" [ params ] ")" block ;
// generic_params = "<" identifier { "," identifier } ">" ;
// params = identifier { "," identifier } ;  (extended: default values, ...rest)
StmtPtr Parser::parseFunctionDecl() {
    int ln = peek().line, cl = peek().column;
    bool isFn = check(TokenType::KW_FN);
    advance(); // consume 'function' or 'fn'
    auto node = std::make_unique<FunctionDecl>();
    node->line = ln; node->col = cl;
    node->isFn = isFn;
    node->name = expect(TokenType::IDENTIFIER, "expected function name").lexeme;

    if (match({TokenType::LT})) {
        node->generics.push_back(expect(TokenType::IDENTIFIER, "expected generic type parameter").lexeme);
        while (match({TokenType::COMMA}))
            node->generics.push_back(expect(TokenType::IDENTIFIER, "expected generic type parameter").lexeme);
        expect(TokenType::GT, "expected '>' to close generic parameter list");
    }

    expect(TokenType::LPAREN, "expected '(' after function name");
    if (!check(TokenType::RPAREN)) {
        do {
            Param p;
            if (match({TokenType::ELLIPSIS})) p.isRest = true; // Extension #20 rest params
            p.name = expect(TokenType::IDENTIFIER, "expected parameter name").lexeme;
            if (match({TokenType::ASSIGN})) p.defaultValue = parseExpression(); // Extension #19
            node->params.push_back(std::move(p));
        } while (match({TokenType::COMMA}));
    }
    expect(TokenType::RPAREN, "expected ')' after parameter list");
    node->body = parseBlock();
    return node;
}

// class_decl = "class" identifier [ ":" identifier ] ( "{" member* "}" | ":" NEWLINE INDENT member* DEDENT ) ;
// A member is a bare field name (optionally = default) or a nested function_decl.
StmtPtr Parser::parseClassDecl() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_CLASS, "expected 'class'");
    auto node = std::make_unique<ClassDecl>();
    node->line = ln; node->col = cl;
    node->name = expect(TokenType::IDENTIFIER, "expected class name").lexeme;
    // Disambiguate ':' meaning inheritance vs. indent-block-start: inheritance
    // is followed directly by a base-class identifier on the same line, while
    // a block-opening ':' is followed by NEWLINE.
    if (check(TokenType::COLON) && peek(1).type == TokenType::IDENTIFIER) {
        advance();
        node->baseName = expect(TokenType::IDENTIFIER, "expected base class name after ':'").lexeme;
    }

    skipNewlines();
    bool brace = check(TokenType::LBRACE);
    if (brace) advance(); else expect(TokenType::COLON, "expected ':' or '{' to open class body");
    skipNewlines();
    if (!brace && check(TokenType::INDENT)) advance();
    skipNewlines();

    auto atEnd = [&]() { return brace ? check(TokenType::RBRACE) : (check(TokenType::DEDENT) || isAtEnd()); };
    while (!atEnd()) {
        ClassMember m;
        if (check(TokenType::KW_FUNCTION) || check(TokenType::KW_FN)) {
            auto fn = parseFunctionDecl();
            m.isMethod = true;
            m.method.reset(static_cast<FunctionDecl*>(fn.release()));
        } else {
            m.fieldName = expect(TokenType::IDENTIFIER, "expected field or method in class body").lexeme;
            if (match({TokenType::ASSIGN})) m.fieldDefault = parseExpression();
        }
        node->members.push_back(std::move(m));
        skipStatementEnd();
    }
    if (brace) expect(TokenType::RBRACE, "expected '}' to close class body");
    else if (check(TokenType::DEDENT)) advance();
    return node;
}

// interface_decl = "interface" identifier ( "{" function_sig* "}" | ":" NEWLINE INDENT function_sig* DEDENT ) ;
StmtPtr Parser::parseInterfaceDecl() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_INTERFACE, "expected 'interface'");
    auto node = std::make_unique<InterfaceDecl>();
    node->line = ln; node->col = cl;
    node->name = expect(TokenType::IDENTIFIER, "expected interface name").lexeme;
    skipNewlines();
    bool brace = check(TokenType::LBRACE);
    if (brace) advance(); else expect(TokenType::COLON, "expected ':' or '{' to open interface body");
    skipNewlines();
    if (!brace && check(TokenType::INDENT)) advance();
    skipNewlines();
    auto atEnd = [&]() { return brace ? check(TokenType::RBRACE) : (check(TokenType::DEDENT) || isAtEnd()); };
    while (!atEnd()) {
        expect(TokenType::KW_FUNCTION, "expected 'function' in interface signature");
        FunctionSig sig;
        sig.name = expect(TokenType::IDENTIFIER, "expected function name").lexeme;
        expect(TokenType::LPAREN, "expected '(' after function name");
        if (!check(TokenType::RPAREN)) {
            sig.params.push_back(expect(TokenType::IDENTIFIER, "expected parameter name").lexeme);
            while (match({TokenType::COMMA}))
                sig.params.push_back(expect(TokenType::IDENTIFIER, "expected parameter name").lexeme);
        }
        expect(TokenType::RPAREN, "expected ')' after parameters");
        node->methods.push_back(std::move(sig));
        skipStatementEnd();
    }
    if (brace) expect(TokenType::RBRACE, "expected '}' to close interface body");
    else if (check(TokenType::DEDENT)) advance();
    return node;
}

// enum_decl = "enum" identifier ( "{" identifier { "," identifier } "}" | ":" NEWLINE INDENT identifier { NEWLINE identifier } DEDENT ) ;
StmtPtr Parser::parseEnumDecl() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_ENUM, "expected 'enum'");
    auto node = std::make_unique<EnumDecl>();
    node->line = ln; node->col = cl;
    node->name = expect(TokenType::IDENTIFIER, "expected enum name").lexeme;
    skipNewlines();
    if (check(TokenType::LBRACE)) {
        advance();
        skipNewlines();
        node->values.push_back(expect(TokenType::IDENTIFIER, "expected enum value").lexeme);
        while (match({TokenType::COMMA})) {
            skipNewlines();
            node->values.push_back(expect(TokenType::IDENTIFIER, "expected enum value").lexeme);
        }
        skipNewlines();
        expect(TokenType::RBRACE, "expected '}' to close enum body");
    } else {
        expect(TokenType::COLON, "expected ':' or '{' to open enum body");
        skipNewlines();
        bool indented = check(TokenType::INDENT);
        if (indented) advance();
        skipNewlines();
        while (check(TokenType::IDENTIFIER)) {
            node->values.push_back(advance().lexeme);
            match({TokenType::COMMA});
            skipStatementEnd();
        }
        if (indented && check(TokenType::DEDENT)) advance();
    }
    return node;
}

// struct_decl = "struct" identifier ( "{" identifier* "}" | ":" NEWLINE INDENT identifier* DEDENT ) ;
StmtPtr Parser::parseStructDecl() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_STRUCT, "expected 'struct'");
    auto node = std::make_unique<StructDecl>();
    node->line = ln; node->col = cl;
    node->name = expect(TokenType::IDENTIFIER, "expected struct name").lexeme;
    skipNewlines();
    bool brace = check(TokenType::LBRACE);
    if (brace) advance(); else expect(TokenType::COLON, "expected ':' or '{' to open struct body");
    skipNewlines();
    if (!brace && check(TokenType::INDENT)) advance();
    skipNewlines();
    auto atEnd = [&]() { return brace ? check(TokenType::RBRACE) : (check(TokenType::DEDENT) || isAtEnd()); };
    while (!atEnd()) {
        node->fields.push_back(expect(TokenType::IDENTIFIER, "expected struct field name").lexeme);
        match({TokenType::COMMA});
        skipStatementEnd();
    }
    if (brace) expect(TokenType::RBRACE, "expected '}' to close struct body");
    else if (check(TokenType::DEDENT)) advance();
    return node;
}

// ═══════════════════════════ modules / imports ═══════════════════════════
StmtPtr Parser::parseModuleDecl() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_MODULE, "expected 'module'");
    auto node = std::make_unique<ModuleDecl>();
    node->line = ln; node->col = cl;
    node->name = expect(TokenType::IDENTIFIER, "expected module name").lexeme;
    node->body = parseBlock();
    return node;
}

// import_stmt = "import" ( qualified_name | "<" identifier ">" ) ;
StmtPtr Parser::parseImportStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_IMPORT, "expected 'import'");
    auto node = std::make_unique<ImportStmt>();
    node->line = ln; node->col = cl;
    if (match({TokenType::LT})) {
        node->isForeignPackage = true;
        std::string path;
        while (!check(TokenType::GT) && !isAtEnd()) path += advance().lexeme + " ";
        expect(TokenType::GT, "expected '>' to close foreign package import");
        node->path = path;
    } else {
        std::string qname = expectIdentifierLike("expected module path after 'import'").lexeme;
        while (match({TokenType::DOT})) {
            qname += "." + expectIdentifierLike("expected identifier after '.' in import path").lexeme;
        }
        node->path = qname;
    }
    return node;
}

StmtPtr Parser::parseExportStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_EXPORT, "expected 'export'");
    auto node = std::make_unique<ExportStmt>();
    node->line = ln; node->col = cl;
    node->name = expect(TokenType::IDENTIFIER, "expected identifier after 'export'").lexeme;
    return node;
}

// ═══════════════════════════ error handling / concurrency / events ═══════════════════════════
// try_stmt = "try" block "catch" "(" identifier ")" block [ "finally" block ] ;
StmtPtr Parser::parseTryStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_TRY, "expected 'try'");
    auto node = std::make_unique<TryStmt>();
    node->line = ln; node->col = cl;
    node->tryBlock = parseBlock();
    skipNewlines();
    expect(TokenType::KW_CATCH, "expected 'catch' after try block");
    expect(TokenType::LPAREN, "expected '(' after 'catch'");
    node->catchVar = expect(TokenType::IDENTIFIER, "expected exception identifier").lexeme;
    expect(TokenType::RPAREN, "expected ')' after catch identifier");
    node->catchBlock = parseBlock();
    skipNewlines();
    if (check(TokenType::KW_FINALLY)) {
        advance();
        node->finallyBlock = parseBlock();
    }
    return node;
}

// thread_stmt = "thread" identifier block ;
StmtPtr Parser::parseThreadStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_THREAD, "expected 'thread'");
    auto node = std::make_unique<ThreadStmt>();
    node->line = ln; node->col = cl;
    node->name = expect(TokenType::IDENTIFIER, "expected thread name").lexeme;
    node->body = parseBlock();
    return node;
}

// async_stmt = "async" function_decl ;
StmtPtr Parser::parseAsyncStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_ASYNC, "expected 'async'");
    auto fn = parseFunctionDecl();
    auto* f = static_cast<FunctionDecl*>(fn.get());
    f->isAsync = true;
    f->line = ln; f->col = cl;
    return fn;
}

// event_stmt = "on" "." identifier block ;
StmtPtr Parser::parseEventStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_ON, "expected 'on'");
    expect(TokenType::DOT, "expected '.' after 'on'");
    auto node = std::make_unique<EventStmt>();
    node->line = ln; node->col = cl;
    node->eventName = expect(TokenType::IDENTIFIER, "expected event name after 'on.'").lexeme;
    node->body = parseBlock();
    return node;
}

// unsafe_stmt = "unsafe" block ;
StmtPtr Parser::parseUnsafeStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_UNSAFE, "expected 'unsafe'");
    auto node = std::make_unique<UnsafeStmt>();
    node->line = ln; node->col = cl;
    node->body = parseBlock();
    return node;
}

// ═══════════════════════════ 25 super-syntax extension statements ═══════════════════════════

// Ext #8: signal score = 0
StmtPtr Parser::parseSignalDecl() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_SIGNAL, "expected 'signal'");
    auto node = std::make_unique<SignalDecl>();
    node->line = ln; node->col = cl;
    node->name = expect(TokenType::IDENTIFIER, "expected signal name").lexeme;
    expect(TokenType::ASSIGN, "expected '=' after signal name");
    node->initial = parseExpression();
    return node;
}

// Ext #13: using f = open("file.txt") block
StmtPtr Parser::parseUsingStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_USING, "expected 'using'");
    auto node = std::make_unique<UsingStmt>();
    node->line = ln; node->col = cl;
    node->varName = expect(TokenType::IDENTIFIER, "expected resource variable name").lexeme;
    expect(TokenType::ASSIGN, "expected '=' after using variable");
    node->resource = parseExpression();
    if (check(TokenType::LBRACE) || check(TokenType::COLON)) {
        node->body = parseBlock();
    }
    return node;
}

// Ext #16: guard x > 0 else { return }
StmtPtr Parser::parseGuardStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_GUARD, "expected 'guard'");
    auto node = std::make_unique<GuardStmt>();
    node->line = ln; node->col = cl;
    node->condition = parseExpression();
    expect(TokenType::KW_ELSE, "expected 'else' after guard condition");
    node->elseBlock = parseBlock();
    return node;
}

// Ext #21: type Matrix = Array<Array<Float>>
StmtPtr Parser::parseTypeAliasStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_TYPE, "expected 'type'");
    auto node = std::make_unique<TypeAliasStmt>();
    node->line = ln; node->col = cl;
    node->aliasName = expect(TokenType::IDENTIFIER, "expected type alias name").lexeme;
    expect(TokenType::ASSIGN, "expected '=' after type alias name");
    node->target = parseTypeRef();
    return node;
}

// Ext #22: extend String { fn reverse() { ... } }
StmtPtr Parser::parseExtendStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_EXTEND, "expected 'extend'");
    auto node = std::make_unique<ExtendStmt>();
    node->line = ln; node->col = cl;
    node->typeName = expect(TokenType::IDENTIFIER, "expected type name after 'extend'").lexeme;
    skipNewlines();
    bool brace = check(TokenType::LBRACE);
    if (brace) advance(); else expect(TokenType::COLON, "expected ':' or '{' to open extend body");
    skipNewlines();
    if (!brace && check(TokenType::INDENT)) advance();
    skipNewlines();
    auto atEnd = [&]() { return brace ? check(TokenType::RBRACE) : (check(TokenType::DEDENT) || isAtEnd()); };
    while (!atEnd()) {
        auto fn = parseFunctionDecl();
        node->methods.emplace_back(static_cast<FunctionDecl*>(fn.release()));
        skipStatementEnd();
    }
    if (brace) expect(TokenType::RBRACE, "expected '}' to close extend body");
    else if (check(TokenType::DEDENT)) advance();
    return node;
}

// Ext #24: lazy config = loadConfig()
StmtPtr Parser::parseLazyDecl() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_LAZY, "expected 'lazy'");
    auto node = std::make_unique<LazyDecl>();
    node->line = ln; node->col = cl;
    node->name = expect(TokenType::IDENTIFIER, "expected lazy variable name").lexeme;
    expect(TokenType::ASSIGN, "expected '=' after lazy variable name");
    node->initializer = parseExpression();
    return node;
}

// Ext #25: comptime { ... }
StmtPtr Parser::parseComptimeStmt() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_COMPTIME, "expected 'comptime'");
    auto node = std::make_unique<ComptimeStmt>();
    node->line = ln; node->col = cl;
    node->body = parseBlock();
    return node;
}

// macro LOG(message): Nova.log.info(message)
StmtPtr Parser::parseMacroDecl() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_MACRO, "expected 'macro'");
    auto node = std::make_unique<MacroDecl>();
    node->line = ln; node->col = cl;
    node->name = expect(TokenType::IDENTIFIER, "expected macro name").lexeme;
    expect(TokenType::LPAREN, "expected '(' after macro name");
    if (!check(TokenType::RPAREN)) {
        node->params.push_back(expect(TokenType::IDENTIFIER, "expected macro parameter").lexeme);
        while (match({TokenType::COMMA}))
            node->params.push_back(expect(TokenType::IDENTIFIER, "expected macro parameter").lexeme);
    }
    expect(TokenType::RPAREN, "expected ')' after macro parameters");
    node->body = parseBlock();
    return node;
}

// Ext #14: chan<T> name  — native concurrency channel declaration.
StmtPtr Parser::parseChanDecl() {
    int ln = peek().line, cl = peek().column;
    expect(TokenType::KW_CHAN, "expected 'chan'");
    auto node = std::make_unique<ChanDecl>();
    node->line = ln; node->col = cl;
    if (match({TokenType::LT})) {
        node->elementType = parseTypeRef();
        expect(TokenType::GT, "expected '>' to close chan element type");
    }
    node->name = expect(TokenType::IDENTIFIER, "expected channel variable name").lexeme;
    return node;
}

// ═══════════════════════════════════════════════════════════════════════
// EXPRESSIONS — precedence climbing chain (Grammar 1 §expression rules,
// extended with the 25 Super-Syntax Extensions: pipe |>, elvis ?:, null
// coalesce ??, smart ranges a..b step s, safe-nav ?., spread ..., etc.)
//
// expression        = assignment_expr
// assignment_expr    = ternary_expr
// ternary_expr       = elvis_coalesce_expr [ "?" expression ":" expression ]
// elvis_coalesce_expr= pipe_expr ( ("?:" | "??") pipe_expr )*
// pipe_expr          = logical_or_expr ( "|>" logical_or_expr )*
// logical_or_expr    = logical_and_expr ( "||" logical_and_expr )*
// logical_and_expr   = equality_expr ( "&&" equality_expr )*
// equality_expr      = relational_expr ( ("=="|"!=") relational_expr )*
// relational_expr    = range_expr ( ("<"|">"|"<="|">=") range_expr )*
// range_expr         = additive_expr [ ".." additive_expr [ "step" additive_expr ] ]
// additive_expr      = multiplicative_expr ( ("+"|"-") multiplicative_expr )*
// multiplicative_expr= unary_expr ( ("*"|"/"|"%"|"^") unary_expr )*
// unary_expr         = ("!"|"-"|"++"|"--"|"await") postfix_expr
// postfix_expr       = primary_expr ( "." id | "?." id | "(" args ")" | "[" slice "]"
//                                    | "!" | "++" | "--" | "as" ["?"] Type | "exists" )*
// ═══════════════════════════════════════════════════════════════════════

ExprPtr Parser::parseExpression() { return parseAssignmentExpr(); }
ExprPtr Parser::parseAssignmentExpr() { return parseTernaryExpr(); }

ExprPtr Parser::parseTernaryExpr() {
    ExprPtr cond = parseElvisOrCoalesceExpr();
    if (match({TokenType::QUESTION})) {
        auto node = std::make_unique<TernaryExpr>();
        node->line = cond->line;
        node->cond = std::move(cond);
        node->thenExpr = parseExpression();
        expect(TokenType::COLON, "expected ':' in ternary expression");
        node->elseExpr = parseExpression();
        return node;
    }
    return cond;
}

ExprPtr Parser::parseElvisOrCoalesceExpr() {
    ExprPtr left = parsePipeExpr();
    while (check(TokenType::ELVIS) || check(TokenType::NULL_COALESCE)) {
        bool isElvis = check(TokenType::ELVIS);
        advance();
        ExprPtr right = parsePipeExpr();
        if (isElvis) {
            auto node = std::make_unique<ElvisExpr>();
            node->line = left->line;
            node->left = std::move(left);
            node->fallback = std::move(right);
            left = std::move(node);
        } else {
            auto node = std::make_unique<NullCoalesceExpr>();
            node->line = left->line;
            node->left = std::move(left);
            node->fallback = std::move(right);
            left = std::move(node);
        }
    }
    return left;
}

// Ext #2: Pipe Operator |>
ExprPtr Parser::parsePipeExpr() {
    ExprPtr left = parseLogicalOrExpr();
    while (check(TokenType::PIPE)) {
        advance();
        auto node = std::make_unique<BinaryExpr>();
        node->line = left->line;
        node->op = "|>";
        node->left = std::move(left);
        node->right = parseLogicalOrExpr();
        left = std::move(node);
    }
    return left;
}

ExprPtr Parser::parseLogicalOrExpr() {
    ExprPtr left = parseLogicalAndExpr();
    while (check(TokenType::OR_OR)) {
        advance();
        auto node = std::make_unique<BinaryExpr>();
        node->line = left->line; node->op = "||";
        node->left = std::move(left); node->right = parseLogicalAndExpr();
        left = std::move(node);
    }
    return left;
}

ExprPtr Parser::parseLogicalAndExpr() {
    ExprPtr left = parseEqualityExpr();
    while (check(TokenType::AND_AND)) {
        advance();
        auto node = std::make_unique<BinaryExpr>();
        node->line = left->line; node->op = "&&";
        node->left = std::move(left); node->right = parseEqualityExpr();
        left = std::move(node);
    }
    return left;
}

ExprPtr Parser::parseEqualityExpr() {
    ExprPtr left = parseRelationalExpr();
    while (check(TokenType::EQ) || check(TokenType::NEQ)) {
        std::string op = advance().lexeme;
        auto node = std::make_unique<BinaryExpr>();
        node->line = left->line; node->op = op;
        node->left = std::move(left); node->right = parseRelationalExpr();
        left = std::move(node);
    }
    return left;
}

ExprPtr Parser::parseRelationalExpr() {
    ExprPtr left = parseRangeExpr();
    while (check(TokenType::LT) || check(TokenType::GT) || check(TokenType::LE) || check(TokenType::GE)) {
        std::string op = advance().lexeme;
        auto node = std::make_unique<BinaryExpr>();
        node->line = left->line; node->op = op;
        node->left = std::move(left); node->right = parseRangeExpr();
        left = std::move(node);
    }
    return left;
}

// Ext #5: Smart Range Iteration  a..b [step s]
ExprPtr Parser::parseRangeExpr() {
    ExprPtr left = parseAdditiveExpr();
    if (check(TokenType::RANGE)) {
        advance();
        auto node = std::make_unique<RangeExpr>();
        node->line = left->line;
        node->from = std::move(left);
        node->to = parseAdditiveExpr();
        if (match({TokenType::KW_STEP})) node->step = parseAdditiveExpr();
        return node;
    }
    return left;
}

ExprPtr Parser::parseAdditiveExpr() {
    ExprPtr left = parseMultiplicativeExpr();
    while (check(TokenType::PLUS) || check(TokenType::MINUS)) {
        std::string op = advance().lexeme;
        auto node = std::make_unique<BinaryExpr>();
        node->line = left->line; node->op = op;
        node->left = std::move(left); node->right = parseMultiplicativeExpr();
        left = std::move(node);
    }
    return left;
}

ExprPtr Parser::parseMultiplicativeExpr() {
    ExprPtr left = parseUnaryExpr();
    while (check(TokenType::STAR) || check(TokenType::SLASH) || check(TokenType::PERCENT) || check(TokenType::CARET)) {
        std::string op = advance().lexeme;
        auto node = std::make_unique<BinaryExpr>();
        node->line = left->line; node->op = op;
        node->left = std::move(left); node->right = parseUnaryExpr();
        left = std::move(node);
    }
    return left;
}

ExprPtr Parser::parseUnaryExpr() {
    if (check(TokenType::BANG) || check(TokenType::MINUS) ||
        check(TokenType::INCREMENT) || check(TokenType::DECREMENT)) {
        std::string op = advance().lexeme;
        auto node = std::make_unique<UnaryExpr>();
        node->line = previous().line; node->op = op; node->prefix = true;
        node->operand = parseUnaryExpr();
        return node;
    }
    if (check(TokenType::KW_AWAIT)) {
        advance();
        auto node = std::make_unique<UnaryExpr>();
        node->line = previous().line; node->op = "await"; node->prefix = true;
        node->operand = parseUnaryExpr();
        return node;
    }
    // Ext #20: spread operator ...args (as a unary prefix in expression position)
    if (check(TokenType::ELLIPSIS)) {
        advance();
        auto node = std::make_unique<SpreadExpr>();
        node->line = previous().line;
        node->target = parseUnaryExpr();
        return node;
    }
    return parsePostfixExpr();
}

// postfix_expr = primary_expr { "." identifier | "(" [ args ] ")" | "?." identifier | "??" expression }
// extended with slicing [a:b:c], indexing [i], postfix !/++/--, 'as'/'as?' casts, 'exists'.
ExprPtr Parser::parsePostfixExpr() {
    ExprPtr expr = parsePrimaryExpr();

    for (;;) {
        if (check(TokenType::DOT)) {
            advance();
            auto node = std::make_unique<MemberExpr>();
            node->line = expr->line;
            node->target = std::move(expr);
            node->name = expect(TokenType::IDENTIFIER, "expected identifier after '.'").lexeme;
            expr = std::move(node);
        } else if (check(TokenType::SAFE_NAV)) {
            advance();
            auto node = std::make_unique<MemberExpr>();
            node->line = expr->line;
            node->target = std::move(expr);
            node->safeNav = true;
            node->name = expect(TokenType::IDENTIFIER, "expected identifier after '?.'").lexeme;
            expr = std::move(node);
        } else if (check(TokenType::LPAREN)) {
            advance();
            auto node = std::make_unique<CallExpr>();
            node->line = expr->line;
            node->callee = std::move(expr);
            if (!check(TokenType::RPAREN)) node->args = parseArgs();
            expect(TokenType::RPAREN, "expected ')' after call arguments");
            expr = std::move(node);
        } else if (check(TokenType::LBRACKET)) {
            advance();
            ExprPtr startE, endE, stepE;
            bool isSlice = false;
            if (!check(TokenType::COLON) && !check(TokenType::RBRACKET)) startE = parseExpression();
            if (check(TokenType::COLON)) {
                isSlice = true;
                advance();
                if (!check(TokenType::COLON) && !check(TokenType::RBRACKET)) endE = parseExpression();
                if (check(TokenType::COLON)) {
                    advance();
                    if (!check(TokenType::RBRACKET)) stepE = parseExpression();
                }
            }
            expect(TokenType::RBRACKET, "expected ']' to close index/slice");
            if (isSlice) {
                auto node = std::make_unique<SliceExpr>();
                node->line = expr->line;
                node->target = std::move(expr);
                node->start = std::move(startE);
                node->end = std::move(endE);
                node->step = std::move(stepE);
                expr = std::move(node);
            } else {
                auto node = std::make_unique<IndexExpr>();
                node->line = expr->line;
                node->target = std::move(expr);
                node->index = std::move(startE);
                expr = std::move(node);
            }
        } else if (check(TokenType::KW_AS)) {
            advance();
            auto node = std::make_unique<AsCastExpr>();
            node->line = expr->line;
            node->safe = match({TokenType::QUESTION}); // as?  (Extension #11)
            node->typeName = expect(TokenType::IDENTIFIER, "expected type name after 'as'").lexeme;
            node->target = std::move(expr);
            expr = std::move(node);
        } else if (check(TokenType::KW_EXISTS)) {
            advance();
            auto node = std::make_unique<ExistsExpr>();
            node->line = expr->line;
            node->target = std::move(expr);
            expr = std::move(node);
        } else if (check(TokenType::INCREMENT) || check(TokenType::DECREMENT)) {
            std::string op = advance().lexeme;
            auto node = std::make_unique<UnaryExpr>();
            node->line = expr->line; node->op = op; node->prefix = false;
            node->operand = std::move(expr);
            expr = std::move(node);
        } else if (check(TokenType::BANG)) {
            // Ext #6: Built-in Error Propagation — result!
            advance();
            if (auto* call = dynamic_cast<CallExpr*>(expr.get())) {
                call->errorPropagate = true;
            } else {
                auto node = std::make_unique<UnaryExpr>();
                node->line = expr->line; node->op = "!"; node->prefix = false;
                node->operand = std::move(expr);
                expr = std::move(node);
            }
        } else {
            break;
        }
    }
    return expr;
}

std::vector<ExprPtr> Parser::parseArgs() {
    std::vector<ExprPtr> args;
    args.push_back(parseExpression());
    while (match({TokenType::COMMA})) args.push_back(parseExpression());
    return args;
}

// Heuristic lookahead: does '(' ... ')' at current position form a lambda
// header, i.e. is it immediately followed by '=>' ?
bool Parser::looksLikeLambdaStart() {
    if (!check(TokenType::LPAREN)) return false;
    int depth = 0;
    size_t i = pos;
    for (; i < toks.size(); i++) {
        if (toks[i].type == TokenType::LPAREN) depth++;
        else if (toks[i].type == TokenType::RPAREN) {
            depth--;
            if (depth == 0) { i++; break; }
        }
    }
    return i < toks.size() && toks[i].type == TokenType::ARROW_FAT;
}

// lambda_expr = "(" [ params ] ")" "=>" ( expression | block ) ;
ExprPtr Parser::parseLambdaExpr() {
    auto node = std::make_unique<LambdaExpr>();
    node->line = peek().line;
    expect(TokenType::LPAREN, "expected '(' to start lambda parameters");
    if (!check(TokenType::RPAREN)) {
        node->params.push_back(expect(TokenType::IDENTIFIER, "expected lambda parameter").lexeme);
        while (match({TokenType::COMMA}))
            node->params.push_back(expect(TokenType::IDENTIFIER, "expected lambda parameter").lexeme);
    }
    expect(TokenType::RPAREN, "expected ')' after lambda parameters");
    expect(TokenType::ARROW_FAT, "expected '=>' in lambda expression");
    skipNewlines();
    if (check(TokenType::LBRACE) || check(TokenType::COLON)) {
        node->bodyBlock = parseBlock();
    } else {
        node->bodyExpr = parseExpression();
    }
    return node;
}

// match_expr = "match" "(" expression ")" ( "{" case* default? "}" | ":" NEWLINE INDENT case* default? DEDENT )
ExprPtr Parser::parseMatchExpr() {
    auto node = std::make_unique<MatchExpr>();
    node->line = peek().line;
    expect(TokenType::KW_MATCH, "expected 'match'");
    expect(TokenType::LPAREN, "expected '(' after 'match'");
    node->subject = parseExpression();
    expect(TokenType::RPAREN, "expected ')' after match subject");
    skipNewlines();
    bool brace = check(TokenType::LBRACE);
    if (brace) advance(); else expect(TokenType::COLON, "expected ':' or '{' to open match body");
    skipNewlines();
    if (!brace && check(TokenType::INDENT)) advance();
    skipNewlines();
    auto atEnd = [&]() { return brace ? check(TokenType::RBRACE) : (check(TokenType::DEDENT) || isAtEnd()); };
    while (!atEnd()) {
        if (match({TokenType::KW_CASE})) {
            MatchCase mc;
            mc.pattern = parseExpression();
            expect(TokenType::COLON, "expected ':' after match case pattern");
            mc.body = parseStatement();
            node->cases.push_back(std::move(mc));
        } else if (match({TokenType::KW_DEFAULT})) {
            expect(TokenType::COLON, "expected ':' after 'default'");
            node->defaultBody = parseStatement();
        } else {
            skipNewlines();
            if (atEnd()) break;
            throw ParseError("expected 'case' or 'default' in match body", peek().line, peek().column);
        }
        skipStatementEnd();
    }
    if (brace) expect(TokenType::RBRACE, "expected '}' to close match body");
    else if (check(TokenType::DEDENT)) advance();
    return node;
}

// "[" elements "]"  — array literal (Extension #1 slicing is postfix, handled above).
ExprPtr Parser::parseArrayOrSliceLiteral() {
    auto node = std::make_unique<ArrayLiteralExpr>();
    node->line = peek().line;
    expect(TokenType::LBRACKET, "expected '['");
    if (!check(TokenType::RBRACKET)) {
        node->elements.push_back(parseExpression());
        while (match({TokenType::COMMA})) {
            if (check(TokenType::RBRACKET)) break; // trailing comma tolerance
            node->elements.push_back(parseExpression());
        }
    }
    expect(TokenType::RBRACKET, "expected ']' to close array literal");
    return node;
}

// "{" key:value, ... "}"  — map literal, OR "{k: v for k, v in pairs}" dict comprehension (Ext #17).
ExprPtr Parser::parseMapLiteral() {
    int ln = peek().line;
    expect(TokenType::LBRACE, "expected '{'");
    skipNewlines();
    if (check(TokenType::RBRACE)) {
        advance();
        auto empty = std::make_unique<MapLiteralExpr>();
        empty->line = ln;
        return empty;
    }

    ExprPtr firstKey = parseExpression();
    expect(TokenType::COLON, "expected ':' between map key and value");
    ExprPtr firstVal = parseExpression();

    if (check(TokenType::KW_FOR)) {
        // Dictionary comprehension: {k: v for k, v in pairs}
        advance();
        auto comp = std::make_unique<DictComprehensionExpr>();
        comp->line = ln;
        comp->keyExpr = std::move(firstKey);
        comp->valueExpr = std::move(firstVal);
        comp->loopVars.push_back(expect(TokenType::IDENTIFIER, "expected loop variable").lexeme);
        while (match({TokenType::COMMA}))
            comp->loopVars.push_back(expect(TokenType::IDENTIFIER, "expected loop variable").lexeme);
        expect(TokenType::KW_IN, "expected 'in' in dictionary comprehension");
        comp->iterable = parseExpression();
        if (match({TokenType::KW_IF})) comp->condition = parseExpression();
        skipNewlines();
        expect(TokenType::RBRACE, "expected '}' to close dictionary comprehension");
        return comp;
    }

    auto node = std::make_unique<MapLiteralExpr>();
    node->line = ln;
    node->entries.push_back({std::move(firstKey), std::move(firstVal)});
    skipNewlines();
    while (match({TokenType::COMMA})) {
        skipNewlines();
        if (check(TokenType::RBRACE)) break;
        ExprPtr k = parseExpression();
        expect(TokenType::COLON, "expected ':' between map key and value");
        ExprPtr v = parseExpression();
        node->entries.push_back({std::move(k), std::move(v)});
        skipNewlines();
    }
    skipNewlines();
    expect(TokenType::RBRACE, "expected '}' to close map literal");
    return node;
}

ExprPtr Parser::parsePrimaryExpr() {
    const Token& t = peek();

    switch (t.type) {
        case TokenType::INT_LITERAL: {
            advance();
            auto n = std::make_unique<LiteralExpr>();
            n->line = t.line; n->kind = LiteralKind::Int; n->intValue = t.intValue;
            return n;
        }
        case TokenType::FLOAT_LITERAL: {
            advance();
            auto n = std::make_unique<LiteralExpr>();
            n->line = t.line; n->kind = LiteralKind::Float; n->floatValue = t.floatValue;
            return n;
        }
        case TokenType::STRING_LITERAL: {
            advance();
            auto n = std::make_unique<LiteralExpr>();
            n->line = t.line; n->kind = LiteralKind::String; n->stringValue = t.lexeme;
            return n;
        }
        case TokenType::RAW_STRING_LITERAL: {
            advance();
            auto n = std::make_unique<LiteralExpr>();
            n->line = t.line; n->kind = LiteralKind::RawString; n->stringValue = t.lexeme;
            return n;
        }
        case TokenType::TRUE_LIT: {
            advance();
            auto n = std::make_unique<LiteralExpr>();
            n->line = t.line; n->kind = LiteralKind::Bool; n->boolValue = true;
            return n;
        }
        case TokenType::FALSE_LIT: {
            advance();
            auto n = std::make_unique<LiteralExpr>();
            n->line = t.line; n->kind = LiteralKind::Bool; n->boolValue = false;
            return n;
        }
        case TokenType::NULL_LIT: {
            advance();
            auto n = std::make_unique<LiteralExpr>();
            n->line = t.line; n->kind = LiteralKind::Null;
            return n;
        }
        case TokenType::DOLLAR_PARAM: {
            // Ext #10: Lambda shorthand $0, $1 used as an implicit-parameter identifier.
            advance();
            auto n = std::make_unique<IdentifierExpr>();
            n->line = t.line; n->name = t.lexeme;
            return n;
        }
        case TokenType::IDENTIFIER: {
            // Paren-less single-parameter lambda shorthand: `user => user.age>=18`
            if (peek(1).type == TokenType::ARROW_FAT) {
                auto node = std::make_unique<LambdaExpr>();
                node->line = t.line;
                node->params.push_back(advance().lexeme); // param name
                advance(); // consume '=>'
                skipNewlines();
                if (check(TokenType::LBRACE) || check(TokenType::COLON)) node->bodyBlock = parseBlock();
                else node->bodyExpr = parseExpression();
                return node;
            }
            advance();
            auto n = std::make_unique<IdentifierExpr>();
            n->line = t.line; n->name = t.lexeme;
            return n;
        }
        case TokenType::KW_NOVA_NS: case TokenType::KW_OPS: case TokenType::KW_OSN:
        case TokenType::KW_OKL: case TokenType::KW_ASTER_LOWER: case TokenType::KW_ASTER_UPPER:
        case TokenType::KW_ESTER: case TokenType::KW_SELF:
        case TokenType::KW_LIST: case TokenType::KW_SET: case TokenType::KW_TUPLE: case TokenType::KW_MAP: {
            advance();
            auto n = std::make_unique<IdentifierExpr>();
            n->line = t.line; n->name = t.lexeme;
            return n;
        }
        case TokenType::KW_MATCH:
            return parseMatchExpr();
        case TokenType::LBRACKET:
            return parseArrayOrSliceLiteral();
        case TokenType::LBRACE:
            return parseMapLiteral();
        case TokenType::LPAREN:
            if (looksLikeLambdaStart()) return parseLambdaExpr();
            {
                advance(); // consume '('
                ExprPtr first = parseExpression();
                if (check(TokenType::COMMA)) {
                    auto tup = std::make_unique<TupleLiteralExpr>();
                    tup->line = t.line;
                    tup->elements.push_back(std::move(first));
                    while (match({TokenType::COMMA})) {
                        if (check(TokenType::RPAREN)) break;
                        tup->elements.push_back(parseExpression());
                    }
                    expect(TokenType::RPAREN, "expected ')' to close tuple literal");
                    return tup;
                }
                expect(TokenType::RPAREN, "expected ')' to close grouped expression");
                return first;
            }
        default:
            throw ParseError("unexpected token in expression: '" + t.lexeme + "' (" +
                              tokenTypeName(t.type) + ")", t.line, t.column);
    }
}

} // namespace nova
