// SafetyAnalyzer.hpp — Nova Safe Mode (Ownership, Borrowing & Basic Lifetimes)
#pragma once

#include "AST.hpp"
#include "Semantic.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

namespace nova {

enum class VarState { Valid, Moved, Borrowed };

class SafetyAnalyzer {
public:
    SafetyAnalyzer(SemanticAnalyzer& semantic, SemErrorSystem& errors);
    bool analyze(const Program& program);

private:
    struct Binding;

    struct BorrowInfo {
        std::size_t immutableCount = 0;
        Binding* mutableBorrow = nullptr;
    };

    struct Binding {
        std::string name;
        VarState state = VarState::Valid;
        bool isMutable = true;
        bool isParameter = false;
        bool isReference = false;
        bool referenceMutable = false;
        Binding* referent = nullptr;
        int scopeDepth = 0;
        int declLine = 0;
        int declCol = 0;
        BorrowInfo borrows;
    };

    struct ScopeFrame {
        std::unordered_map<std::string, Binding*> bindings;
        std::vector<Binding*> ownedBindings;
    };

    struct FunctionFrame {
        std::string name;
        int depth = 0;
    };

    SemanticAnalyzer& semantic;
    SemErrorSystem& errors;

    // Publicly meaningful ownership state retained from the original analyzer.
    // The lexical binding table below is authoritative when shadowing exists.
    std::unordered_map<std::string, VarState> varStates;

    std::unordered_set<std::string> primitiveTypes = {
        "Int", "Float", "Bool", "String"
    };

    std::vector<ScopeFrame> scopes;
    std::vector<std::unique_ptr<Binding>> bindings;
    std::vector<FunctionFrame> functions;

    void checkStmt(const Stmt* stmt);
    void checkExpr(const Expr* expr, bool isAssignmentTarget = false);

    void pushScope();
    void popScope();
    Binding* declareBinding(const std::string& name, bool isMutable,
                           bool isParameter, int line, int col);
    Binding* lookupBinding(const std::string& name) const;

    void enterFunction(const FunctionDecl* fn);
    void leaveFunction();

    bool isPrimitive(const std::string& typeName) const;
    bool isBorrowOp(const UnaryExpr* unary) const;
    bool isMutableBorrowOp(const UnaryExpr* unary) const;

    bool validateIdentifierUse(const IdentifierExpr* id);
    bool validateBorrow(const UnaryExpr* unary);
    bool validateAssignmentTarget(const Expr* target);
    bool validateMutation(Binding* binding, int line, int col,
                          const std::string& operation);

    void acquireImmutableBorrow(Binding* referent, Binding* owner,
                                int line, int col);
    void acquireMutableBorrow(Binding* referent, Binding* owner,
                              int line, int col);
    void releaseBorrow(Binding* owner);

    bool resolveReferenceInitializer(const Expr* expr, Binding*& referent,
                                    bool& mutableReference);
    bool checkReturnReference(const Expr* expr, int line, int col);
    bool referencesLocalToCurrentFunction(Binding* referent) const;
    bool isLiveBinding(const Binding* binding) const;

    void checkBlock(const Block* block, bool createScope = true);
};

} // namespace nova
