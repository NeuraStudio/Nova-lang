import os

# Update Semantic.hpp
with open('Semantic.hpp', 'r') as f:
    hpp = f.read()
if 'bool isPublic = false;' not in hpp:
    hpp = hpp.replace('bool isMutable = true;', 'bool isMutable = true;\n    bool isPublic = false; // Visibility Checker')
    hpp = hpp.replace('ScopeManager scopeMgr;', 'ScopeManager scopeMgr;\n    bool nextSymbolIsPublic = false;')
with open('Semantic.hpp', 'w') as f:
    f.write(hpp)

# Update Semantic.cpp
with open('Semantic.cpp', 'r') as f:
    cpp = f.read()

if 'sym.isPublic = nextSymbolIsPublic;' not in cpp:
    # declareSymbol update
    cpp = cpp.replace('bool SemanticAnalyzer::declareSymbol(Symbol sym) {\n    sym.scopeDepth', 'bool SemanticAnalyzer::declareSymbol(Symbol sym) {\n    sym.isPublic = nextSymbolIsPublic;\n    nextSymbolIsPublic = false;\n    sym.scopeDepth')
    
    # visitAnnotated update
    old_annotated = """void SemanticAnalyzer::visitAnnotated(const AnnotatedStmt* a) {
    for (auto& ann : a->annotations)
        for (auto& arg : ann.args) visitExpr(arg.get());
    visitStmt(a->inner.get());
}"""
    new_annotated = """void SemanticAnalyzer::visitAnnotated(const AnnotatedStmt* a) {
    bool wasPublic = nextSymbolIsPublic;
    for (auto& ann : a->annotations) {
        if (ann.name == "Public") nextSymbolIsPublic = true;
        for (auto& arg : ann.args) visitExpr(arg.get());
    }
    visitStmt(a->inner.get());
    nextSymbolIsPublic = wasPublic;
}"""
    cpp = cpp.replace(old_annotated, new_annotated)

    # visitExport update
    old_export = """} else {
        sym->isUsed = true;
    }"""
    new_export = """} else {
        sym->isUsed = true;
        if (!sym->isPublic) {
            errSys.error(e->line, e->col, "E2010", "cannot export '" + e->name + "': symbol is private. Add @Public annotation.");
        }
    }"""
    cpp = cpp.replace(old_export, new_export)

with open('Semantic.cpp', 'w') as f:
    f.write(cpp)

print("Semantic Analyzer successfully upgraded for Visibility Checking!")
