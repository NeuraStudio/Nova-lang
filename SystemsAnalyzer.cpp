// SystemsAnalyzer.cpp — Enforces strict rules for Raw Pointers, Allocations & Memory Layouts
#include "SystemsAnalyzer.hpp"

namespace nova {

SystemsAnalyzer::SystemsAnalyzer(SemErrorSystem& errors) : errors(errors), unsafeDepth(0) {}

bool SystemsAnalyzer::analyze(const Program& program) {
    for (auto& stmt : program.statements) {
        visitStmt(stmt.get());
    }
    return !errors.hasErrors();
}

std::string SystemsAnalyzer::getExprStr(const Expr* e) {
    if (auto* id = dynamic_cast<const IdentifierExpr*>(e)) return id->name;
    if (auto* m = dynamic_cast<const MemberExpr*>(e)) return getExprStr(m->target.get()) + "." + m->name;
    return "";
}

void SystemsAnalyzer::visitStmt(const Stmt* stmt) {
    if (!stmt) return;
    
    if (auto* u = dynamic_cast<const UnsafeStmt*>(stmt)) {
        unsafeDepth++; // We are entering System/Unsafe Mode!
        if (u->body) {
            for (auto& s : u->body->statements) visitStmt(s.get());
        }
        unsafeDepth--; // Exiting System Mode
    } 
    else if (auto* b = dynamic_cast<const Block*>(stmt)) {
        for (auto& s : b->statements) visitStmt(s.get());
    } 
    else if (auto* d = dynamic_cast<const DeclarationStmt*>(stmt)) {
        visitExpr(d->value.get());
    } 
    else if (auto* a = dynamic_cast<const AssignmentStmt*>(stmt)) {
        visitExpr(a->target.get());
        visitExpr(a->value.get());
    } 
    else if (auto* e = dynamic_cast<const ExprStmt*>(stmt)) {
        visitExpr(e->expr.get());
    }
}

void SystemsAnalyzer::visitExpr(const Expr* expr) {
    if (!expr) return;
    
    if (auto* c = dynamic_cast<const CallExpr*>(expr)) {
        std::string name = getExprStr(c->callee.get());
        
        // INTERCEPTING LOW-LEVEL MEMORY & POINTER ARITHMETIC
        if (name == "Nova.memory.allocate" || name == "Nova.memory.free" || 
            name == "Ops.addressOf" || name == "Ops.deref" || name == "Ops.ptr_add") {
            
            if (unsafeDepth == 0) {
                // VIOLATION: Memory operation outside unsafe block
                errors.error(c->line, c->col, "E4002", "Systems Mode Violation: Low-level memory operation '" + name + "' requires an 'unsafe' block.");
            }
        }
        visitExpr(c->callee.get());
        for (auto& arg : c->args) visitExpr(arg.get());
    } 
    else if (auto* b = dynamic_cast<const BinaryExpr*>(expr)) {
        visitExpr(b->left.get());
        visitExpr(b->right.get());
    } 
    else if (auto* m = dynamic_cast<const MemberExpr*>(expr)) {
        visitExpr(m->target.get());
    }
}

} // namespace nova
