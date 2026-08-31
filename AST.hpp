// AST.hpp — Nova Language Abstract Syntax Tree node definitions.
// One node class per production in Grammar 1 (EBNF). Every node keeps
// source line/col for diagnostics. Uses smart pointers for ownership.
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace nova {

struct Node { virtual ~Node() = default; int line = 0; int col = 0; };
struct Expr : Node {};
struct Stmt : Node {};

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// A "block" is just a list of statements — represents both brace_block and
// indent_block, since Grammar 1 says both resolve to identical AST nodes.
struct Block : Node {
    std::vector<StmtPtr> statements;
};
using BlockPtr = std::unique_ptr<Block>;

// ══════════════════════════ EXPRESSIONS ══════════════════════════

enum class LiteralKind { Int, Float, String, RawString, Bool, Null };
struct LiteralExpr : Expr {
    LiteralKind kind;
    std::string stringValue;
    int64_t intValue = 0;
    double floatValue = 0.0;
    bool boolValue = false;
};

struct IdentifierExpr : Expr {
    std::string name;
};

// arr[start:end:step] — Extension #1 (advanced slicing)
struct SliceExpr : Expr {
    ExprPtr target;
    ExprPtr start;  // nullable
    ExprPtr end;    // nullable
    ExprPtr step;   // nullable
};

struct IndexExpr : Expr {   // arr[expr]
    ExprPtr target;
    ExprPtr index;
};

struct MemberExpr : Expr {  // a.b
    ExprPtr target;
    std::string name;
    bool safeNav = false;   // a?.b   (Extension #23)
};

struct CallExpr : Expr {    // f(args)
    ExprPtr callee;
    std::vector<ExprPtr> args;
    bool errorPropagate = false; // result!()  (Extension #6)
};

struct UnaryExpr : Expr {
    std::string op;  // "!" "-" "++" "--"
    ExprPtr operand;
    bool prefix = true;
};

struct BinaryExpr : Expr {
    std::string op;  // + - * / % ^ == != < > <= >= && || |>
    ExprPtr left;
    ExprPtr right;
};

struct TernaryExpr : Expr { // cond ? then : else
    ExprPtr cond, thenExpr, elseExpr;
};

struct ElvisExpr : Expr {   // val ?: default   (Extension #3)
    ExprPtr left, fallback;
};

struct NullCoalesceExpr : Expr { // val ?? default
    ExprPtr left, fallback;
};

struct RangeExpr : Expr {   // a..b  or a..b step s   (Extension #5)
    ExprPtr from, to, step; // step nullable
};

struct LambdaExpr : Expr {  // (params) => expr | block   (also supports $0,$1 shorthand)
    std::vector<std::string> params;
    ExprPtr bodyExpr;   // set if expression-bodied
    BlockPtr bodyBlock; // set if block-bodied
    bool usesShorthandParams = false; // $0 $1 style, params inferred
};

struct SpreadExpr : Expr {  // ...args   (Extension #20)
    ExprPtr target;
};

struct ArrayLiteralExpr : Expr {
    std::vector<ExprPtr> elements;
};

struct MapEntry { ExprPtr key; ExprPtr value; };
struct MapLiteralExpr : Expr {
    std::vector<MapEntry> entries;
};

// {k: v for k, v in pairs}   (Extension #17 dictionary comprehension)
struct DictComprehensionExpr : Expr {
    ExprPtr keyExpr, valueExpr;
    std::vector<std::string> loopVars;
    ExprPtr iterable;
    ExprPtr condition; // optional filter, nullable
};

struct TupleLiteralExpr : Expr {
    std::vector<ExprPtr> elements;
};

struct MatchCase { ExprPtr pattern; StmtPtr body; };
struct MatchExpr : Expr {   // match(expr) { case ...: stmt ... default: stmt }
    ExprPtr subject;
    std::vector<MatchCase> cases;
    StmtPtr defaultBody; // nullable
};

struct AsCastExpr : Expr {  // value as Type   /   value as? Type  (Extension #11)
    ExprPtr target;
    std::string typeName;
    bool safe = false;
};

struct ExistsExpr : Expr {  // value exists   (used in "if value exists:")
    ExprPtr target;
};

// ══════════════════════════ TYPE ANNOTATIONS ══════════════════════════

struct TypeRef {
    std::string name;               // e.g. "List", "Int", "Matrix"
    std::vector<TypeRef> generics;  // e.g. List<Array<Float>>
};

// ══════════════════════════ STATEMENTS ══════════════════════════

struct DeclarationStmt : Stmt {  // [const] identifier = expression
    bool isConst = false;
    bool isMut = false;
    std::string name;
    std::optional<TypeRef> typeAnnotation; // for List<T> x, Set y, etc.
    ExprPtr value; // nullable for `List users` style bare declarations
};

struct AssignmentStmt : Stmt {   // lvalue = expr   (also += -= *= /= tuple-swap)
    ExprPtr target;     // IdentifierExpr | MemberExpr | IndexExpr
    std::string op;     // "=" "+=" "-=" "*=" "/="
    ExprPtr value;
};

// a, b = b, a   (Extension #18 tuple swapping) and destructuring `let [x,y] = point`
struct DestructuringStmt : Stmt {
    std::vector<std::string> targets;
    ExprPtr value;
    bool isArrayPattern = false; // [x, y] = ... vs a, b = ...
};

struct ExprStmt : Stmt { ExprPtr expr; };

struct IfStmt : Stmt {
    ExprPtr condition;
    BlockPtr thenBranch;
    BlockPtr elseBranch;      // nullable
    std::unique_ptr<IfStmt> elseIf; // for elif chains, nullable
};

struct CaseClause { ExprPtr value; std::vector<StmtPtr> body; bool hasBreak = false; };
struct SwitchStmt : Stmt {
    ExprPtr subject;
    std::vector<CaseClause> cases;
    std::vector<StmtPtr> defaultBody;
    bool hasDefault = false;
};

struct ForStmt : Stmt {   // for i = a to b [step s] block
    std::string varName;
    ExprPtr from, to, step; // step nullable
    BlockPtr body;
};

struct WhileStmt : Stmt { ExprPtr condition; BlockPtr body; };
struct RepeatStmt : Stmt { ExprPtr count; BlockPtr body; };
struct ForeachStmt : Stmt { std::string varName; ExprPtr iterable; BlockPtr body; };

struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};
struct ReturnStmt : Stmt { ExprPtr value; /* nullable */ };
struct YieldStmt : Stmt { ExprPtr value; };

struct Param { std::string name; ExprPtr defaultValue; /* nullable, Extension #19 */ bool isRest = false; };
struct FunctionDecl : Stmt {
    std::string name;
    bool isFn = false; // true if declared with 'fn' rather than 'function'
    std::vector<std::string> generics;
    std::vector<Param> params;
    BlockPtr body;
    bool isAsync = false;
};

struct ClassMember {
    // A member is either a field (bare identifier[, default]) or a method (FunctionDecl).
    bool isMethod = false;
    std::string fieldName;
    ExprPtr fieldDefault; // nullable
    std::unique_ptr<FunctionDecl> method; // set if isMethod
};
struct ClassDecl : Stmt {
    std::string name;
    std::string baseName; // empty if none
    std::vector<ClassMember> members;
};

struct FunctionSig { std::string name; std::vector<std::string> params; };
struct InterfaceDecl : Stmt {
    std::string name;
    std::vector<FunctionSig> methods;
};

struct EnumDecl : Stmt {
    std::string name;
    std::vector<std::string> values;
};

struct StructField { std::string name; };
struct StructDecl : Stmt {
    std::string name;
    std::vector<std::string> fields;
};

struct ModuleDecl : Stmt { std::string name; BlockPtr body; };
struct ImportStmt : Stmt { std::string path; bool isForeignPackage = false; /* import <pkg> */ };
struct ExportStmt : Stmt { std::string name; };

struct TryStmt : Stmt {
    BlockPtr tryBlock;
    std::string catchVar;
    BlockPtr catchBlock;
    BlockPtr finallyBlock; // nullable
};

struct ThreadStmt : Stmt { std::string name; BlockPtr body; };
struct EventStmt : Stmt { std::string eventName; BlockPtr body; }; // on.start: ...
struct UnsafeStmt : Stmt { BlockPtr body; };

struct SignalDecl : Stmt { std::string name; ExprPtr initial; };              // Ext #8
struct UsingStmt : Stmt { std::string varName; ExprPtr resource; BlockPtr body; }; // Ext #13
struct GuardStmt : Stmt { ExprPtr condition; BlockPtr elseBlock; };            // Ext #16
struct TypeAliasStmt : Stmt { std::string aliasName; TypeRef target; };        // Ext #21
struct ExtendStmt : Stmt { std::string typeName; std::vector<std::unique_ptr<FunctionDecl>> methods; }; // Ext #22
struct LazyDecl : Stmt { std::string name; ExprPtr initializer; };             // Ext #24
struct ComptimeStmt : Stmt { BlockPtr body; };                                 // Ext #25
struct MacroDecl : Stmt { std::string name; std::vector<std::string> params; BlockPtr body; };
struct ChanDecl : Stmt { std::string name; TypeRef elementType; };             // Ext #14

// Annotation like @Public / @Optimize("O3") attached to the following declaration.
struct Annotation { std::string name; std::vector<ExprPtr> args; };
struct AnnotatedStmt : Stmt {
    std::vector<Annotation> annotations;
    StmtPtr inner;
};

struct Program : Node {
    std::vector<StmtPtr> statements;
};

} // namespace nova
