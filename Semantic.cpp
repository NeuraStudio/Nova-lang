// Semantic.cpp — Nova Language Semantic Analyzer implementation.
#include "Semantic.hpp"
#include <algorithm>
#include <sstream>

namespace nova {

// ═══════════════════════════════ 5. Semantic Error System ═══════════════════════════════

void SemErrorSystem::report(std::ostream& out) const {
    std::vector<SemDiagnostic> sorted = diags;
    std::stable_sort(sorted.begin(), sorted.end(), [](const SemDiagnostic& a, const SemDiagnostic& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.col < b.col;
    });
    for (const auto& d : sorted) {
        const char* tag = d.severity == SemSeverity::Error ? "error"
                         : d.severity == SemSeverity::Warning ? "warning" : "note";
        out << "[" << tag << (d.code.empty() ? "" : (" " + d.code)) << "] "
            << "line " << d.line << ", col " << d.col << ": " << d.message << "\n";
    }
    out << "\n" << errorCount << " error(s), " << warningCount << " warning(s)\n";
}

// ═══════════════════════════════ 1. Symbol Table ═══════════════════════════════

bool SymbolTable::declare(int scopeId, const Symbol& sym) {
    if (lookupLocal(scopeId, sym.name) != nullptr) return false;
    storage.push_back(sym);
    scopeIndex[scopeId].push_back(storage.size() - 1);
    return true;
}

Symbol* SymbolTable::lookupLocal(int scopeId, const std::string& name) {
    auto it = scopeIndex.find(scopeId);
    if (it == scopeIndex.end()) return nullptr;
    for (size_t idx : it->second) {
        if (storage[idx].name == name) return &storage[idx];
    }
    return nullptr;
}

Symbol* SymbolTable::lookup(const std::vector<int>& scopeChain, const std::string& name) {
    for (int scopeId : scopeChain) {
        if (Symbol* s = lookupLocal(scopeId, name)) return s;
    }
    return nullptr;
}

std::vector<Symbol*> SymbolTable::symbolsInScope(int scopeId) {
    std::vector<Symbol*> out;
    auto it = scopeIndex.find(scopeId);
    if (it == scopeIndex.end()) return out;
    for (size_t idx : it->second) out.push_back(&storage[idx]);
    return out;
}

Symbol* SymbolTable::findGlobalClass(const std::string& name) {
    for (auto& s : storage) {
        if (s.name == name && (s.kind == SymbolKind::Class || s.kind == SymbolKind::Struct ||
                                s.kind == SymbolKind::Interface || s.kind == SymbolKind::Enum)) {
            return &s;
        }
    }
    return nullptr;
}

bool SymbolTable::setConstValue(const std::string& name, int declLine, int declCol, const std::string& value) {
    for (auto& s : storage) {
        if (s.name == name && s.kind == SymbolKind::Constant &&
            s.declLine == declLine && s.declCol == declCol) {
            s.hasConstValue = true;
            s.constValue = value;
            return true;
        }
    }
    return false;
}

// ═══════════════════════════════ 2. Scope Manager ═══════════════════════════════

ScopeManager::ScopeManager() {
    scopes.push_back({0, -1, ScopeKind::Global, "<global>"});
    scopeStack.push_back(0);
    nextId = 1;
}

int ScopeManager::push(ScopeKind kind, const std::string& label) {
    int parent = scopeStack.back();
    int id = nextId++;
    scopes.push_back({id, parent, kind, label});
    scopeStack.push_back(id);
    return id;
}

void ScopeManager::pop() {
    if (scopeStack.size() > 1) scopeStack.pop_back();
}

std::vector<int> ScopeManager::currentChain() const {
    std::vector<int> chain;
    int id = scopeStack.back();
    while (id != -1) {
        chain.push_back(id);
        id = scopes[id].parentId;
    }
    return chain;
}

int ScopeManager::nearestEnclosing(ScopeKind kind) const {
    int id = scopeStack.back();
    while (id != -1) {
        if (scopes[id].kind == kind) return id;
        id = scopes[id].parentId;
    }
    return -1;
}

// ═══════════════════════════════ 4. Import Resolver ═══════════════════════════════

ImportResolver::ImportResolver() {
    // Known Nova standard-library namespaces (Grammar 1 worked examples: import Nova.ai,
    // import Nova.web, import Nova.database, plus the runtime root namespaces from
    // Grammar 2 §5). Extend this list as the real Nova stdlib surface grows.
    novaStdlibModules = {
        "Nova.ai", "Nova.web", "Nova.database", "Nova.robotics", "Nova.network",
        "Nova.file", "Nova.memory", "Nova.cloud", "Nova.math", "Nova.io",
        "Nova.crypto", "Nova.gui", "Nova.game", "Nova.ml"
    };
}

void ImportResolver::registerUserModule(const std::string& name, int line, int col) {
    userModules[name] = {line, col};
}

ResolvedImport ImportResolver::resolve(const ImportStmt& stmt, SemErrorSystem& errors) {
    ResolvedImport result;
    result.path = stmt.path;
    result.isForeignPackage = stmt.isForeignPackage;
    result.isKnownNovaModule = false;
    result.resolved = false;

    if (stmt.isForeignPackage) {
        // import <any language PKG> — Foreign Language Package Bridge (Grammar 1 §8).
        // Always considered resolvable at the syntax level; actual linkage is a
        // build-system concern outside the semantic analyzer's scope.
        result.resolved = !stmt.path.empty();
        if (!result.resolved) {
            errors.error(stmt.line, stmt.col, "E1101", "empty foreign package import '<>' — expected a package name");
        }
        return result;
    }

    if (stmt.path.empty()) {
        errors.error(stmt.line, stmt.col, "E1102", "malformed import statement: missing module path");
        return result;
    }

    // Known Nova.* stdlib namespace?
    for (const auto& mod : novaStdlibModules) {
        if (stmt.path == mod) { result.isKnownNovaModule = true; result.resolved = true; return result; }
    }

    // User-declared module in this same program (module Foo: ... / export Foo)?
    if (userModules.count(stmt.path)) { result.resolved = true; return result; }

    // A dotted path starting with "Nova." that isn't in our known list is still
    // treated as part of the (larger, evolving) Nova runtime surface — warn,
    // don't hard-error, since the stdlib is a moving target during development.
    if (stmt.path.rfind("Nova.", 0) == 0) {
        errors.warning(stmt.line, stmt.col, "W1103",
                        "'" + stmt.path + "' is not a recognized Nova standard-library module "
                        "(will be treated as valid — update ImportResolver's known-module list if this is stdlib)");
        result.resolved = true;
        return result;
    }

    // Anything else (single bare identifier, custom dotted path) is assumed to be
    // a user/project-local module that may simply not have been declared yet in
    // this compilation unit (e.g. lives in another file) — warn, not error.
    errors.warning(stmt.line, stmt.col, "W1104",
                    "import '" + stmt.path + "' does not match any module declared in this file; "
                    "assuming it resolves in another compilation unit");
    result.resolved = true;
    return result;
}

// ═══════════════════════════════ 3. Name Resolver + top-level Analyzer ═══════════════════════════════

SemanticAnalyzer::SemanticAnalyzer() {
    // Grammar 2 §5 — Nova runtime root namespaces are always in scope,
    // without requiring a user declaration or import.
    builtinNamespaces = {"Nova", "Ops", "osn", "okl", "aster", "Aster", "ester", "self"};
}

bool SemanticAnalyzer::isBuiltinNamespace(const std::string& name) const {
    return std::find(builtinNamespaces.begin(), builtinNamespaces.end(), name) != builtinNamespaces.end();
}

bool SemanticAnalyzer::declareSymbol(Symbol sym) {
    sym.scopeDepth = static_cast<int>(scopeMgr.currentChain().size()) - 1;
    int scopeId = scopeMgr.currentScopeId();
    if (!symTab.declare(scopeId, sym)) {
        Symbol* existing = symTab.lookupLocal(scopeId, sym.name);
        errSys.error(sym.declLine, sym.declCol, "E2001",
                     "redeclaration of '" + sym.name + "' in the same scope"
                     + (existing ? " (first declared at line " + std::to_string(existing->declLine) + ")" : ""));
        return false;
    }
    return true;
}

bool SemanticAnalyzer::analyze(const Program& program) {
    // Pre-pass: register every top-level `module` so ImportResolver can
    // resolve intra-file imports regardless of declaration order.
    for (auto& s : program.statements) {
        if (auto* m = dynamic_cast<const ModuleDecl*>(s.get())) {
            importResolver.registerUserModule(m->name, m->line, m->col);
        } else if (auto* ann = dynamic_cast<const AnnotatedStmt*>(s.get())) {
            if (auto* m2 = dynamic_cast<const ModuleDecl*>(ann->inner.get())) {
                importResolver.registerUserModule(m2->name, m2->line, m2->col);
            }
        }
    }

    for (auto& s : program.statements) {
        visitStmt(s.get());
    }

    // Post-pass: warn about declared-but-never-used locals in the global
    // scope (a light, genuinely useful check to ship with Step 1).
    for (Symbol* sym : symTab.symbolsInScope(scopeMgr.globalScopeId())) {
        if (!sym->isUsed && (sym->kind == SymbolKind::Variable || sym->kind == SymbolKind::Constant)) {
            errSys.warning(sym->declLine, sym->declCol, "W2101",
                            "'" + sym->name + "' is declared but never used");
        }
    }

    return !errSys.hasErrors();
}

// ─────────────────────────── statement dispatch ───────────────────────────

void SemanticAnalyzer::visitBlock(const Block* block, ScopeKind kind, const std::string& label) {
    if (!block) return;
    scopeMgr.push(kind, label);
    for (auto& s : block->statements) visitStmt(s.get());
    scopeMgr.pop();
}

void SemanticAnalyzer::visitStmt(const Stmt* stmt) {
    if (!stmt) return;

    if (auto* n = dynamic_cast<const DeclarationStmt*>(stmt)) { visitDeclaration(n); return; }
    if (auto* n = dynamic_cast<const AssignmentStmt*>(stmt)) { visitAssignment(n); return; }
    if (auto* n = dynamic_cast<const DestructuringStmt*>(stmt)) { visitDestructuring(n); return; }
    if (auto* n = dynamic_cast<const ExprStmt*>(stmt)) { visitExprStmt(n); return; }
    if (auto* n = dynamic_cast<const IfStmt*>(stmt)) { visitIf(n); return; }
    if (auto* n = dynamic_cast<const SwitchStmt*>(stmt)) { visitSwitch(n); return; }
    if (auto* n = dynamic_cast<const ForStmt*>(stmt)) { visitFor(n); return; }
    if (auto* n = dynamic_cast<const WhileStmt*>(stmt)) { visitWhile(n); return; }
    if (auto* n = dynamic_cast<const RepeatStmt*>(stmt)) { visitRepeat(n); return; }
    if (auto* n = dynamic_cast<const ForeachStmt*>(stmt)) { visitForeach(n); return; }
    if (auto* n = dynamic_cast<const BreakStmt*>(stmt)) { visitBreakContinue(n, true); return; }
    if (auto* n = dynamic_cast<const ContinueStmt*>(stmt)) { visitBreakContinue(n, false); return; }
    if (auto* n = dynamic_cast<const ReturnStmt*>(stmt)) { visitReturn(n); return; }
    if (auto* n = dynamic_cast<const YieldStmt*>(stmt)) { visitYield(n); return; }
    if (auto* n = dynamic_cast<const FunctionDecl*>(stmt)) { visitFunctionDecl(n); return; }
    if (auto* n = dynamic_cast<const ClassDecl*>(stmt)) { visitClassDecl(n); return; }
    if (auto* n = dynamic_cast<const InterfaceDecl*>(stmt)) { visitInterfaceDecl(n); return; }
    if (auto* n = dynamic_cast<const EnumDecl*>(stmt)) { visitEnumDecl(n); return; }
    if (auto* n = dynamic_cast<const StructDecl*>(stmt)) { visitStructDecl(n); return; }
    if (auto* n = dynamic_cast<const ModuleDecl*>(stmt)) { visitModuleDecl(n); return; }
    if (auto* n = dynamic_cast<const ImportStmt*>(stmt)) { visitImport(n); return; }
    if (auto* n = dynamic_cast<const ExportStmt*>(stmt)) { visitExport(n); return; }
    if (auto* n = dynamic_cast<const TryStmt*>(stmt)) { visitTry(n); return; }
    if (auto* n = dynamic_cast<const ThreadStmt*>(stmt)) { visitThread(n); return; }
    if (auto* n = dynamic_cast<const EventStmt*>(stmt)) { visitEvent(n); return; }
    if (auto* n = dynamic_cast<const UnsafeStmt*>(stmt)) { visitUnsafe(n); return; }
    if (auto* n = dynamic_cast<const SignalDecl*>(stmt)) { visitSignal(n); return; }
    if (auto* n = dynamic_cast<const UsingStmt*>(stmt)) { visitUsing(n); return; }
    if (auto* n = dynamic_cast<const GuardStmt*>(stmt)) { visitGuard(n); return; }
    if (auto* n = dynamic_cast<const TypeAliasStmt*>(stmt)) { visitTypeAlias(n); return; }
    if (auto* n = dynamic_cast<const ExtendStmt*>(stmt)) { visitExtend(n); return; }
    if (auto* n = dynamic_cast<const LazyDecl*>(stmt)) { visitLazy(n); return; }
    if (auto* n = dynamic_cast<const ComptimeStmt*>(stmt)) { visitComptime(n); return; }
    if (auto* n = dynamic_cast<const MacroDecl*>(stmt)) { visitMacro(n); return; }
    if (auto* n = dynamic_cast<const ChanDecl*>(stmt)) { visitChan(n); return; }
    if (auto* n = dynamic_cast<const AnnotatedStmt*>(stmt)) { visitAnnotated(n); return; }

    errSys.note(stmt->line, stmt->col, "N9000", "semantic analyzer: unhandled statement node (skipped)");
}

// ─────────────────────────── declarations / assignment ───────────────────────────

void SemanticAnalyzer::visitDeclaration(const DeclarationStmt* d) {
    if (d->value) visitExpr(d->value.get());
    Symbol sym;
    sym.name = d->name;
    sym.kind = d->isConst ? SymbolKind::Constant : SymbolKind::Variable;
    sym.declLine = d->line; sym.declCol = d->col;
    sym.isMutable = !d->isConst;
    if (d->typeAnnotation) sym.typeName = d->typeAnnotation->name;
    declareSymbol(sym);
}

void SemanticAnalyzer::visitAssignment(const AssignmentStmt* a) {
    // Nova allows implicit declaration on first bare assignment — the spec's
    // own examples show `score = 100` as a *declaration*, not an error
    // (Grammar 1: `declaration = [ "const" ] identifier "=" expression`).
    // The parser emits AssignmentStmt for any non-const/let/var `x = expr`,
    // so the analyzer is responsible for telling "first write == declare"
    // apart from "write to an already-known name". Only a genuinely
    // undeclared identifier used as the root of a MEMBER/INDEX target
    // (e.g. `foo.bar = 1` where `foo` was never introduced) is an error.
    const Expr* root = a->target.get();
    bool isPlainIdentifierTarget = dynamic_cast<const IdentifierExpr*>(root) != nullptr;
    while (true) {
        if (auto* m = dynamic_cast<const MemberExpr*>(root)) { root = m->target.get(); continue; }
        if (auto* ix = dynamic_cast<const IndexExpr*>(root)) { root = ix->target.get(); continue; }
        break;
    }

    // Resolve the RHS first (matches DeclarationStmt ordering: value is
    // evaluated in the scope *before* the new name comes into existence).
    visitExpr(a->value.get());
    if (!isPlainIdentifierTarget) visitExpr(a->target.get());

    if (auto* id = dynamic_cast<const IdentifierExpr*>(root)) {
        auto chain = scopeMgr.currentChain();
        Symbol* sym = symTab.lookup(chain, id->name);

        if (!sym && !isBuiltinNamespace(id->name)) {
            if (isPlainIdentifierTarget && a->op == "=") {
                // Implicit declaration: `name = value` introduces `name`.
                Symbol newSym;
                newSym.name = id->name;
                newSym.kind = SymbolKind::Variable;
                newSym.declLine = a->line; newSym.declCol = a->col;
                newSym.isUsed = true; // an assignment target isn't "unused"
                declareSymbol(newSym);
            } else if (isPlainIdentifierTarget) {
                // Compound assignment (+= -= *= /=) or ++/-- on a name that
                // was never introduced — that genuinely has no prior value.
                errSys.error(a->line, a->col, "E2002",
                             "compound assignment to undeclared identifier '" + id->name + "'");
            } else {
                errSys.error(a->line, a->col, "E2002",
                             "assignment through undeclared identifier '" + id->name + "'");
            }
        } else if (sym) {
            sym->isUsed = true;
            if (a->target.get() == root && sym->kind == SymbolKind::Constant) {
                errSys.error(a->line, a->col, "E2003",
                             "cannot assign to '" + id->name + "' — declared 'const'");
            }
        }
    }
}

void SemanticAnalyzer::visitDestructuring(const DestructuringStmt* d) {
    visitExpr(d->value.get());
    for (auto& name : d->targets) {
        // Tuple-swap form (`a, b = b, a`) targets often already exist —
        // declare only if genuinely new in this scope, otherwise treat as
        // a reassignment (mark used, no redeclare error).
        int scopeId = scopeMgr.currentScopeId();
        if (Symbol* existing = symTab.lookupLocal(scopeId, name)) {
            existing->isUsed = true;
            continue;
        }
        Symbol sym;
        sym.name = name;
        sym.kind = SymbolKind::Variable;
        sym.declLine = d->line; sym.declCol = d->col;
        declareSymbol(sym);
    }
}

void SemanticAnalyzer::visitExprStmt(const ExprStmt* e) { visitExpr(e->expr.get()); }

// ─────────────────────────── control flow ───────────────────────────

void SemanticAnalyzer::visitIf(const IfStmt* i) {
    visitExpr(i->condition.get());
    visitBlock(i->thenBranch.get(), ScopeKind::Block, "if-then");
    if (i->elseIf) visitStmt(i->elseIf.get());
    if (i->elseBranch) visitBlock(i->elseBranch.get(), ScopeKind::Block, "if-else");
}

void SemanticAnalyzer::visitSwitch(const SwitchStmt* s) {
    visitExpr(s->subject.get());
    for (auto& c : s->cases) {
        visitExpr(c.value.get());
        scopeMgr.push(ScopeKind::Block, "case");
        for (auto& st : c.body) visitStmt(st.get());
        scopeMgr.pop();
    }
    if (s->hasDefault) {
        scopeMgr.push(ScopeKind::Block, "default");
        for (auto& st : s->defaultBody) visitStmt(st.get());
        scopeMgr.pop();
    }
}

void SemanticAnalyzer::visitFor(const ForStmt* f) {
    visitExpr(f->from.get());
    visitExpr(f->to.get());
    if (f->step) visitExpr(f->step.get());
    scopeMgr.push(ScopeKind::Loop, "for");
    Symbol sym; sym.name = f->varName; sym.kind = SymbolKind::Variable;
    sym.declLine = f->line; sym.declCol = f->col; sym.isUsed = true; // loop counters rarely flagged
    declareSymbol(sym);
    for (auto& st : f->body->statements) visitStmt(st.get());
    scopeMgr.pop();
}

void SemanticAnalyzer::visitWhile(const WhileStmt* w) {
    visitExpr(w->condition.get());
    visitBlock(w->body.get(), ScopeKind::Loop, "while");
}

void SemanticAnalyzer::visitRepeat(const RepeatStmt* r) {
    visitExpr(r->count.get());
    visitBlock(r->body.get(), ScopeKind::Loop, "repeat");
}

void SemanticAnalyzer::visitForeach(const ForeachStmt* f) {
    visitExpr(f->iterable.get());
    scopeMgr.push(ScopeKind::Loop, "foreach");
    Symbol sym; sym.name = f->varName; sym.kind = SymbolKind::Variable;
    sym.declLine = f->line; sym.declCol = f->col; sym.isUsed = true;
    declareSymbol(sym);
    for (auto& st : f->body->statements) visitStmt(st.get());
    scopeMgr.pop();
}

void SemanticAnalyzer::visitBreakContinue(const Stmt* s, bool isBreak) {
    if (scopeMgr.nearestEnclosing(ScopeKind::Loop) == -1) {
        errSys.error(s->line, s->col, isBreak ? "E2004" : "E2005",
                     std::string("'") + (isBreak ? "break" : "continue") + "' used outside of a loop");
    }
}

void SemanticAnalyzer::visitReturn(const ReturnStmt* r) {
    if (fnStack.empty()) {
        errSys.error(r->line, r->col, "E2006", "'return' used outside of a function body");
    }
    if (r->value) visitExpr(r->value.get());
}

void SemanticAnalyzer::visitYield(const YieldStmt* y) {
    if (fnStack.empty()) {
        errSys.error(y->line, y->col, "E2007", "'yield' used outside of a function body");
    }
    visitExpr(y->value.get());
}

// ─────────────────────────── functions / OOP ───────────────────────────

void SemanticAnalyzer::visitFunctionDecl(const FunctionDecl* f, bool asMethod) {
    // Declare the function name itself in the *enclosing* scope before
    // entering its body, so recursive calls resolve correctly.
    if (!asMethod) {
        Symbol sym;
        sym.name = f->name;
        sym.kind = SymbolKind::Function;
        sym.declLine = f->line; sym.declCol = f->col;
        sym.arity = static_cast<int>(f->params.size());
        for (auto& p : f->params) sym.paramNames.push_back(p.name);
        declareSymbol(sym);
    }

    scopeMgr.push(ScopeKind::Function, f->name);
    fnStack.push_back({true, f->isAsync, f->name});

    for (auto& g : f->generics) {
        Symbol gsym; gsym.name = g; gsym.kind = SymbolKind::TypeAlias;
        gsym.declLine = f->line; gsym.declCol = f->col; gsym.isUsed = true;
        declareSymbol(gsym);
    }
    for (auto& p : f->params) {
        if (p.defaultValue) visitExpr(p.defaultValue.get()); // Ext #19 default params
        Symbol psym; psym.name = p.name; psym.kind = SymbolKind::Parameter;
        psym.declLine = f->line; psym.declCol = f->col; psym.isUsed = true;
        declareSymbol(psym);
    }
    if (f->body) for (auto& st : f->body->statements) visitStmt(st.get());

    fnStack.pop_back();
    scopeMgr.pop();
}

void SemanticAnalyzer::visitClassDecl(const ClassDecl* c) {
    Symbol sym;
    sym.name = c->name;
    sym.kind = SymbolKind::Class;
    sym.declLine = c->line; sym.declCol = c->col;
    sym.baseClassName = c->baseName;
    for (auto& m : c->members) sym.memberNames.push_back(m.isMethod ? m.method->name : m.fieldName);
    declareSymbol(sym);

    // Base-class existence check (only for locally-declared base classes;
    // a base declared in another file/module is assumed valid here since
    // this analyzer works one compilation unit at a time).
    if (!c->baseName.empty() && symTab.findGlobalClass(c->baseName) == nullptr) {
        errSys.warning(c->line, c->col, "W2102",
                        "base class '" + c->baseName + "' for '" + c->name +
                        "' was not found in this file (may be declared elsewhere)");
    }

    scopeMgr.push(ScopeKind::Class, c->name);
    for (auto& m : c->members) {
        if (m.isMethod) {
            visitFunctionDecl(m.method.get(), /*asMethod=*/true);
        } else {
            if (m.fieldDefault) visitExpr(m.fieldDefault.get());
            Symbol fieldSym; fieldSym.name = m.fieldName; fieldSym.kind = SymbolKind::Field;
            fieldSym.declLine = c->line; fieldSym.declCol = c->col; fieldSym.isUsed = true;
            declareSymbol(fieldSym);
        }
    }
    scopeMgr.pop();
}

void SemanticAnalyzer::visitInterfaceDecl(const InterfaceDecl* i) {
    Symbol sym;
    sym.name = i->name; sym.kind = SymbolKind::Interface;
    sym.declLine = i->line; sym.declCol = i->col;
    for (auto& m : i->methods) sym.memberNames.push_back(m.name);
    declareSymbol(sym);
}

void SemanticAnalyzer::visitEnumDecl(const EnumDecl* e) {
    Symbol sym;
    sym.name = e->name; sym.kind = SymbolKind::Enum;
    sym.declLine = e->line; sym.declCol = e->col;
    sym.memberNames = e->values;
    declareSymbol(sym);

    // Enum values are also registered as symbols in the enclosing scope
    // (dotless-access styles like `Online` in `enum Status { Online, ... }`
    // used bare in match/switch cases per the worked examples).
    for (auto& v : e->values) {
        Symbol vsym; vsym.name = v; vsym.kind = SymbolKind::EnumValue;
        vsym.declLine = e->line; vsym.declCol = e->col; vsym.isUsed = true;
        vsym.typeName = e->name;
        declareSymbol(vsym);
    }
}

void SemanticAnalyzer::visitStructDecl(const StructDecl* s) {
    Symbol sym;
    sym.name = s->name; sym.kind = SymbolKind::Struct;
    sym.declLine = s->line; sym.declCol = s->col;
    sym.memberNames = s->fields;
    declareSymbol(sym);
}

// ─────────────────────────── modules / imports ───────────────────────────

void SemanticAnalyzer::visitModuleDecl(const ModuleDecl* m) {
    Symbol sym;
    sym.name = m->name; sym.kind = SymbolKind::Module;
    sym.declLine = m->line; sym.declCol = m->col;
    declareSymbol(sym);
    visitBlock(m->body.get(), ScopeKind::Module, m->name);
}

void SemanticAnalyzer::visitImport(const ImportStmt* i) {
    importResolver.resolve(*i, errSys);
}

void SemanticAnalyzer::visitExport(const ExportStmt* e) {
    auto chain = scopeMgr.currentChain();
    Symbol* sym = symTab.lookup(chain, e->name);
    if (!sym) {
        errSys.error(e->line, e->col, "E2008",
                     "cannot export '" + e->name + "': no such symbol is declared in this scope");
    } else {
        sym->isUsed = true;
    }
}

// ─────────────────────────── error handling / concurrency / events ───────────────────────────

void SemanticAnalyzer::visitTry(const TryStmt* t) {
    visitBlock(t->tryBlock.get(), ScopeKind::Block, "try");
    scopeMgr.push(ScopeKind::Block, "catch");
    Symbol errSym; errSym.name = t->catchVar; errSym.kind = SymbolKind::Variable;
    errSym.declLine = t->line; errSym.declCol = t->col; errSym.isUsed = true;
    declareSymbol(errSym);
    for (auto& st : t->catchBlock->statements) visitStmt(st.get());
    scopeMgr.pop();
    if (t->finallyBlock) visitBlock(t->finallyBlock.get(), ScopeKind::Block, "finally");
}

void SemanticAnalyzer::visitThread(const ThreadStmt* t) {
    visitBlock(t->body.get(), ScopeKind::Block, "thread:" + t->name);
}

void SemanticAnalyzer::visitEvent(const EventStmt* e) {
    visitBlock(e->body.get(), ScopeKind::Block, "on." + e->eventName);
}

void SemanticAnalyzer::visitUnsafe(const UnsafeStmt* u) {
    visitBlock(u->body.get(), ScopeKind::Block, "unsafe");
}

// ─────────────────────────── 25 super-syntax extension statements ───────────────────────────

void SemanticAnalyzer::visitSignal(const SignalDecl* s) {
    visitExpr(s->initial.get());
    Symbol sym; sym.name = s->name; sym.kind = SymbolKind::Signal;
    sym.declLine = s->line; sym.declCol = s->col;
    declareSymbol(sym);
}

void SemanticAnalyzer::visitUsing(const UsingStmt* u) {
    visitExpr(u->resource.get());
    scopeMgr.push(ScopeKind::Block, "using:" + u->varName);
    Symbol sym; sym.name = u->varName; sym.kind = SymbolKind::Variable;
    sym.declLine = u->line; sym.declCol = u->col; sym.isUsed = true;
    declareSymbol(sym);
    if (u->body) for (auto& st : u->body->statements) visitStmt(st.get());
    scopeMgr.pop();
}

void SemanticAnalyzer::visitGuard(const GuardStmt* g) {
    visitExpr(g->condition.get());
    visitBlock(g->elseBlock.get(), ScopeKind::Block, "guard-else");
}

void SemanticAnalyzer::visitTypeAlias(const TypeAliasStmt* t) {
    Symbol sym; sym.name = t->aliasName; sym.kind = SymbolKind::TypeAlias;
    sym.declLine = t->line; sym.declCol = t->col; sym.typeName = t->target.name; sym.isUsed = true;
    declareSymbol(sym);
}

void SemanticAnalyzer::visitExtend(const ExtendStmt* e) {
    // `extend` adds methods to an existing type — it does not declare a new
    // symbol, but each method body is still name-resolved in its own scope.
    scopeMgr.push(ScopeKind::Class, "extend:" + e->typeName);
    for (auto& m : e->methods) visitFunctionDecl(m.get(), /*asMethod=*/true);
    scopeMgr.pop();
}

void SemanticAnalyzer::visitLazy(const LazyDecl* l) {
    visitExpr(l->initializer.get());
    Symbol sym; sym.name = l->name; sym.kind = SymbolKind::Variable;
    sym.declLine = l->line; sym.declCol = l->col;
    declareSymbol(sym);
}

void SemanticAnalyzer::visitComptime(const ComptimeStmt* c) {
    visitBlock(c->body.get(), ScopeKind::Block, "comptime");
}

void SemanticAnalyzer::visitMacro(const MacroDecl* m) {
    Symbol sym; sym.name = m->name; sym.kind = SymbolKind::Macro;
    sym.declLine = m->line; sym.declCol = m->col;
    sym.arity = static_cast<int>(m->params.size());
    sym.paramNames = m->params;
    declareSymbol(sym);

    scopeMgr.push(ScopeKind::Function, "macro:" + m->name);
    for (auto& p : m->params) {
        Symbol psym; psym.name = p; psym.kind = SymbolKind::Parameter;
        psym.declLine = m->line; psym.declCol = m->col; psym.isUsed = true;
        declareSymbol(psym);
    }
    if (m->body) for (auto& st : m->body->statements) visitStmt(st.get());
    scopeMgr.pop();
}

void SemanticAnalyzer::visitChan(const ChanDecl* c) {
    Symbol sym; sym.name = c->name; sym.kind = SymbolKind::Chan;
    sym.declLine = c->line; sym.declCol = c->col; sym.typeName = c->elementType.name;
    declareSymbol(sym);
}

void SemanticAnalyzer::visitAnnotated(const AnnotatedStmt* a) {
    for (auto& ann : a->annotations)
        for (auto& arg : ann.args) visitExpr(arg.get());
    visitStmt(a->inner.get());
}

// ═══════════════════════════════ expression visitor (Name Resolver core) ═══════════════════════════════

void SemanticAnalyzer::resolveIdentifierUse(const IdentifierExpr* id) {
    if (isBuiltinNamespace(id->name)) return; // Grammar 2 §5 — always in scope

    // Ext #10: $0/$1/$2 lambda shorthand params are valid even when used
    // directly as a bare call argument with no enclosing explicit lambda,
    // e.g. `[1,2,3].map($0 * 2)` — the shorthand itself implies the lambda.
    if (id->name.size() >= 2 && id->name[0] == '$' &&
        std::all_of(id->name.begin() + 1, id->name.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
        return;
    }

    auto chain = scopeMgr.currentChain();
    Symbol* sym = symTab.lookup(chain, id->name);
    if (sym) {
        sym->isUsed = true;
    } else {
        errSys.error(id->line, id->col, "E2009", "use of undeclared identifier '" + id->name + "'");
    }
}

void SemanticAnalyzer::visitExpr(const Expr* expr) {
    if (!expr) return;

    if (auto* n = dynamic_cast<const IdentifierExpr*>(expr)) { resolveIdentifierUse(n); return; }
    if (dynamic_cast<const LiteralExpr*>(expr)) return;

    if (auto* n = dynamic_cast<const MemberExpr*>(expr)) { visitExpr(n->target.get()); return; }
    if (auto* n = dynamic_cast<const IndexExpr*>(expr)) { visitExpr(n->target.get()); visitExpr(n->index.get()); return; }
    if (auto* n = dynamic_cast<const SliceExpr*>(expr)) {
        visitExpr(n->target.get());
        if (n->start) visitExpr(n->start.get());
        if (n->end) visitExpr(n->end.get());
        if (n->step) visitExpr(n->step.get());
        return;
    }
    if (auto* n = dynamic_cast<const CallExpr*>(expr)) {
        visitExpr(n->callee.get());
        for (auto& a : n->args) visitExpr(a.get());
        // Arity check when the callee is a plain, resolvable function name.
        if (auto* id = dynamic_cast<const IdentifierExpr*>(n->callee.get())) {
            auto chain = scopeMgr.currentChain();
            Symbol* sym = symTab.lookup(chain, id->name);
            if (sym && sym->kind == SymbolKind::Function && sym->arity >= 0) {
                bool hasRestOrDefaultTail = false; // spread args make exact-count checks unreliable
                for (auto& a : n->args) if (dynamic_cast<const SpreadExpr*>(a.get())) hasRestOrDefaultTail = true;
                if (!hasRestOrDefaultTail && static_cast<int>(n->args.size()) > sym->arity) {
                    errSys.warning(n->line, n->col, "W2103",
                                   "call to '" + id->name + "' passes " + std::to_string(n->args.size()) +
                                   " argument(s) but it's declared with " + std::to_string(sym->arity));
                }
            }
        }
        return;
    }
    if (auto* n = dynamic_cast<const UnaryExpr*>(expr)) { visitExpr(n->operand.get()); return; }
    if (auto* n = dynamic_cast<const BinaryExpr*>(expr)) { visitExpr(n->left.get()); visitExpr(n->right.get()); return; }
    if (auto* n = dynamic_cast<const TernaryExpr*>(expr)) {
        visitExpr(n->cond.get()); visitExpr(n->thenExpr.get()); visitExpr(n->elseExpr.get()); return;
    }
    if (auto* n = dynamic_cast<const ElvisExpr*>(expr)) { visitExpr(n->left.get()); visitExpr(n->fallback.get()); return; }
    if (auto* n = dynamic_cast<const NullCoalesceExpr*>(expr)) { visitExpr(n->left.get()); visitExpr(n->fallback.get()); return; }
    if (auto* n = dynamic_cast<const RangeExpr*>(expr)) {
        visitExpr(n->from.get()); visitExpr(n->to.get());
        if (n->step) visitExpr(n->step.get());
        return;
    }
    if (auto* n = dynamic_cast<const LambdaExpr*>(expr)) {
        scopeMgr.push(ScopeKind::Function, "<lambda>");
        fnStack.push_back({true, false, "<lambda>"});
        for (auto& p : n->params) {
            Symbol psym; psym.name = p; psym.kind = SymbolKind::Parameter;
            psym.declLine = n->line; psym.declCol = n->col; psym.isUsed = true;
            declareSymbol(psym);
        }
        // Ext #10: $0/$1 shorthand params are implicitly in scope inside any
        // lambda body even without an explicit parameter list.
        for (const char* shorthand : {"$0", "$1", "$2"}) {
            Symbol psym; psym.name = shorthand; psym.kind = SymbolKind::Parameter;
            psym.declLine = n->line; psym.declCol = n->col; psym.isUsed = true;
            symTab.declare(scopeMgr.currentScopeId(), psym); // best-effort; ignore dup
        }
        if (n->bodyExpr) visitExpr(n->bodyExpr.get());
        else if (n->bodyBlock) for (auto& st : n->bodyBlock->statements) visitStmt(st.get());
        fnStack.pop_back();
        scopeMgr.pop();
        return;
    }
    if (auto* n = dynamic_cast<const SpreadExpr*>(expr)) { visitExpr(n->target.get()); return; }
    if (auto* n = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
        for (auto& e : n->elements) visitExpr(e.get());
        return;
    }
    if (auto* n = dynamic_cast<const MapLiteralExpr*>(expr)) {
        for (auto& kv : n->entries) { visitExpr(kv.key.get()); visitExpr(kv.value.get()); }
        return;
    }
    if (auto* n = dynamic_cast<const DictComprehensionExpr*>(expr)) {
        visitExpr(n->iterable.get());
        scopeMgr.push(ScopeKind::Block, "dict-comprehension");
        for (auto& v : n->loopVars) {
            Symbol sym; sym.name = v; sym.kind = SymbolKind::Variable;
            sym.declLine = n->line; sym.declCol = n->col; sym.isUsed = true;
            declareSymbol(sym);
        }
        visitExpr(n->keyExpr.get());
        visitExpr(n->valueExpr.get());
        if (n->condition) visitExpr(n->condition.get());
        scopeMgr.pop();
        return;
    }
    if (auto* n = dynamic_cast<const TupleLiteralExpr*>(expr)) {
        for (auto& e : n->elements) visitExpr(e.get());
        return;
    }
    if (auto* n = dynamic_cast<const MatchExpr*>(expr)) {
        visitExpr(n->subject.get());
        for (auto& c : n->cases) {
            visitExpr(c.pattern.get());
            scopeMgr.push(ScopeKind::Block, "match-case");
            visitStmt(c.body.get());
            scopeMgr.pop();
        }
        if (n->defaultBody) {
            scopeMgr.push(ScopeKind::Block, "match-default");
            visitStmt(n->defaultBody.get());
            scopeMgr.pop();
        }
        return;
    }
    if (auto* n = dynamic_cast<const AsCastExpr*>(expr)) { visitExpr(n->target.get()); return; }
    if (auto* n = dynamic_cast<const ExistsExpr*>(expr)) { visitExpr(n->target.get()); return; }

    errSys.note(expr->line, expr->col, "N9001", "semantic analyzer: unhandled expression node (skipped)");
}

} // namespace nova

