// SystemsAnalyzer.hpp — Nova Systems Mode (Low-Level Memory & Pointer Checker)
#pragma once
#include "AST.hpp"
#include "Semantic.hpp"
#include <string>

namespace nova {

class SystemsAnalyzer {
public:
    SystemsAnalyzer(SemErrorSystem& errors);
    bool analyze(const Program& program);

private:
    SemErrorSystem& errors;
    int unsafeDepth; // Tracks if we are inside an `unsafe` block

    void visitStmt(const Stmt* stmt);
    void visitExpr(const Expr* expr);
    
    // Helper to get the full string name of a MemberExpr (e.g. "Nova.memory.allocate")
    std::string getExprStr(const Expr* e);
};

} // namespace nova
