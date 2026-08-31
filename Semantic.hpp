// Semantic.hpp — Nova Language Semantic Analyzer (STEP 1)
// ═══════════════════════════════════════════════════════════════════════
// Walks the AST produced by Parser.cpp and performs:
//   1. Symbol Table   — records every declared name (var/const/fn/class/...)
//   2. Scope Manager   — nested lexical scopes (block/function/class/module)
//   3. Name Resolver    — binds every identifier use to its declaring symbol
//   4. Import Resolver  — resolves `import` paths against known modules
//   5. Semantic Error System — collects diagnostics instead of throwing,
//      so a single run reports every problem it can find.
// ═══════════════════════════════════════════════════════════════════════
#pragma once
#include "AST.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include <ostream>

namespace nova {

// ═══════════════════════════════ 5. Semantic Error System ═══════════════════════════════

enum class SemSeverity { Error, Warning, Note };

struct SemDiagnostic {
    SemSeverity severity;
    std::string message;
    int line;
    int col;
    std::string code; // short machine-friendly code, e.g. "E0001"
};

// Central sink for every diagnostic the analyzer produces. Never throws —
// analysis continues after an error so the user gets a full report in one
// pass, similar to how real-world compilers (rustc, clang) behave.
class SemErrorSystem {
public:
    void error(int line, int col, const std::string& code, const std::string& msg) {
        diags.push_back({SemSeverity::Error, msg, line, col, code});
        errorCount++;
    }
    void warning(int line, int col, const std::string& code, const std::string& msg) {
        diags.push_back({SemSeverity::Warning, msg, line, col, code});
        warningCount++;
    }
    void note(int line, int col, const std::string& code, const std::string& msg) {
        diags.push_back({SemSeverity::Note, msg, line, col, code});
    }

    bool hasErrors() const { return errorCount > 0; }
    int getErrorCount() const { return errorCount; }
    int getWarningCount() const { return warningCount; }
    const std::vector<SemDiagnostic>& all() const { return diags; }

    // Pretty-prints every diagnostic, sorted by source order, to `out`.
    void report(std::ostream& out) const;

private:
    std::vector<SemDiagnostic> diags;
    int errorCount = 0;
    int warningCount = 0;
};

// ═══════════════════════════════ 1. Symbol Table ═══════════════════════════════

enum class SymbolKind {
    Variable, Constant, Function, Class, Interface, Enum, Struct,
    Module, Parameter, Field, EnumValue, Signal, TypeAlias, Chan, Macro
};

struct Symbol {
    std::string name;
    SymbolKind kind;
    int declLine = 0;
    int declCol = 0;
    bool isMutable = true;      // const => false
    bool isUsed = false;        // for unused-variable warnings
    int scopeDepth = 0;
    std::string typeName;
    bool hasConstValue = false;
    std::string constValue;       // best-effort textual type (may be empty/inferred)
    // For functions: arity + param names (used for call-site arg-count checks)
    int arity = -1;
    std::vector<std::string> paramNames;
    // For classes: base class name, if any (used by the Name Resolver to
    // walk inheritance chains when looking up inherited members).
    std::string baseClassName;
    std::vector<std::string> memberNames; // fields + method names, for classes/structs/interfaces
};

// Flat, queryable table of every symbol declared anywhere in the program.
// Scopes are tracked separately by ScopeManager; this table is the
// permanent record used for lookups, diagnostics, and (later) codegen.
class SymbolTable {
public:
    // Registers a new symbol in the given scope id. Returns false (and does
    // NOT insert) if a symbol with the same name already exists in that
    // exact scope — caller is expected to raise a redeclaration error.
    bool declare(int scopeId, const Symbol& sym);

    // Looks up `name` starting in `scopeId` and walking up the given parent
    // chain (provided by ScopeManager). Returns nullptr if not found.
    Symbol* lookup(const std::vector<int>& scopeChain, const std::string& name);

    // Direct lookup within exactly one scope (no walking up).
    Symbol* lookupLocal(int scopeId, const std::string& name);

    // All symbols declared in a given scope (used for unused-variable scans).
    std::vector<Symbol*> symbolsInScope(int scopeId);

    // All symbols across the whole program (used for global reporting / codegen).
    const std::vector<Symbol>& allSymbols() const { return storage; }

    Symbol* findGlobalClass(const std::string& name);
    bool setConstValue(const std::string& name, int declLine, int declCol, const std::string& value);

private:
    // storage owns every symbol; scopes map scopeId -> indices into storage.
    std::vector<Symbol> storage;
    std::unordered_map<int, std::vector<size_t>> scopeIndex;
};

// ═══════════════════════════════ 2. Scope Manager ═══════════════════════════════

enum class ScopeKind { Global, Block, Function, Class, Module, Loop };

struct ScopeInfo {
    int id;
    int parentId; // -1 for the global scope
    ScopeKind kind;
    std::string label; // function/class/module name, for diagnostics
};

// Manages a tree of nested lexical scopes. push()/pop() mirror the AST walk;
// the analyzer pushes a new scope for every block/function/class/module and
// pops it when done, exactly matching Nova's block = brace_block | indent_block.
class ScopeManager {
public:
    ScopeManager();

    int push(ScopeKind kind, const std::string& label = "");
    void pop();

    int currentScopeId() const { return scopeStack.back(); }
    ScopeKind currentKind() const { return scopes[currentScopeId()].kind; }

    // Returns [currentScopeId, parent, grandparent, ..., globalScopeId] —
    // the walk order used by SymbolTable::lookup for name resolution.
    std::vector<int> currentChain() const;

    // Walk up from current scope to nearest enclosing scope of `kind`
    // (used e.g. to validate `break`/`continue` are inside a Loop scope,
    // or `self` is inside a Class scope). Returns -1 if none found.
    int nearestEnclosing(ScopeKind kind) const;

    const ScopeInfo& info(int scopeId) const { return scopes[scopeId]; }
    int globalScopeId() const { return 0; }

private:
    std::vector<ScopeInfo> scopes;   // indexed by scope id
    std::vector<int> scopeStack;     // active nesting path (top = current)
    int nextId = 0;
};

// ═══════════════════════════════ 4. Import Resolver ═══════════════════════════════

struct ResolvedImport {
    std::string path;            // e.g. "Nova.ai", "Nova.database"
    bool isForeignPackage;       // import <pkg>
    bool isKnownNovaModule;      // path starts with a recognized Nova.* stdlib namespace
    bool resolved;                // false if the path/module could not be resolved at all
};

// Resolves `import` statements against Nova's known standard-library
// namespace surface (Nova.ai, Nova.web, Nova.database, ...) and against
// user-declared `module` blocks found elsewhere in the same program.
// Unknown-but-well-formed paths are treated as external/foreign modules
// (resolved=true, isKnownNovaModule=false) rather than hard errors, since
// Nova supports importing arbitrary user modules and foreign packages.
class ImportResolver {
public:
    ImportResolver();

    // Call once per user-declared `module Name { ... }` found during the walk.
    void registerUserModule(const std::string& name, int line, int col);

    // Resolves a single import statement. Emits diagnostics into `errors`
    // for malformed or genuinely unresolvable paths.
    ResolvedImport resolve(const ImportStmt& stmt, SemErrorSystem& errors);

    const std::vector<std::string>& knownNovaModules() const { return novaStdlibModules; }

private:
    std::vector<std::string> novaStdlibModules; // "Nova.ai", "Nova.web", ...
    std::unordered_map<std::string, std::pair<int,int>> userModules; // name -> decl loc
};

// ═══════════════════════════════ 3. Name Resolver + top-level Analyzer ═══════════════════════════════

// Orchestrates the whole semantic pass: walks the Program AST, drives the
// ScopeManager + SymbolTable to declare and resolve every name, resolves
// imports via ImportResolver, and reports everything through SemErrorSystem.
class SemanticAnalyzer {
public:
    SemanticAnalyzer();

    // Runs the full analysis. Returns true if no errors were found
    // (warnings are still allowed). Safe to call once per Program.
    bool analyze(const Program& program);

    SemErrorSystem& errors() { return errSys; }
    SymbolTable& symbols() { return symTab; }
    ScopeManager& scopes() { return scopeMgr; }

private:
    SemErrorSystem errSys;
    SymbolTable symTab;
    ScopeManager scopeMgr;
    ImportResolver importResolver;

    // Tracks whether we're currently inside a function/method body, and
    // whether that function is async — used to validate `return`/`await`.
    struct FnContext { bool inFunction = false; bool isAsync = false; std::string name; };
    std::vector<FnContext> fnStack;

    // Names of built-in runtime namespaces that resolve without user
    // declaration (Grammar 2 §5): Nova, Ops, osn, okl, aster, Aster, ester.
    std::vector<std::string> builtinNamespaces;
    bool isBuiltinNamespace(const std::string& name) const;

    // ---- statement visitors ----
    void visitBlock(const Block* block, ScopeKind kind, const std::string& label = "");
    void visitStmt(const Stmt* stmt);
    void visitDeclaration(const DeclarationStmt* d);
    void visitAssignment(const AssignmentStmt* a);
    void visitDestructuring(const DestructuringStmt* d);
    void visitExprStmt(const ExprStmt* e);
    void visitIf(const IfStmt* i);
    void visitSwitch(const SwitchStmt* s);
    void visitFor(const ForStmt* f);
    void visitWhile(const WhileStmt* w);
    void visitRepeat(const RepeatStmt* r);
    void visitForeach(const ForeachStmt* f);
    void visitBreakContinue(const Stmt* s, bool isBreak);
    void visitReturn(const ReturnStmt* r);
    void visitYield(const YieldStmt* y);
    void visitFunctionDecl(const FunctionDecl* f, bool asMethod = false);
    void visitClassDecl(const ClassDecl* c);
    void visitInterfaceDecl(const InterfaceDecl* i);
    void visitEnumDecl(const EnumDecl* e);
    void visitStructDecl(const StructDecl* s);
    void visitModuleDecl(const ModuleDecl* m);
    void visitImport(const ImportStmt* i);
    void visitExport(const ExportStmt* e);
    void visitTry(const TryStmt* t);
    void visitThread(const ThreadStmt* t);
    void visitEvent(const EventStmt* e);
    void visitUnsafe(const UnsafeStmt* u);
    void visitSignal(const SignalDecl* s);
    void visitUsing(const UsingStmt* u);
    void visitGuard(const GuardStmt* g);
    void visitTypeAlias(const TypeAliasStmt* t);
    void visitExtend(const ExtendStmt* e);
    void visitLazy(const LazyDecl* l);
    void visitComptime(const ComptimeStmt* c);
    void visitMacro(const MacroDecl* m);
    void visitChan(const ChanDecl* c);
    void visitAnnotated(const AnnotatedStmt* a);

    // ---- expression visitors (resolve every identifier use) ----
    void visitExpr(const Expr* expr);
    void resolveIdentifierUse(const IdentifierExpr* id);

    // ---- helpers ----
    bool declareSymbol(Symbol sym); // false + error emitted if duplicate in current scope
    void exportNamesOf(const Stmt* s, std::vector<std::string>& outNames);
};

} // namespace nova
