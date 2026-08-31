// TypeChecker.hpp — Nova Supreme Type Checker
#pragma once
#include "AST.hpp"
#include "Semantic.hpp"
#include "Type.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nova {

class TypeEnv {
public:
    void setType(const Expr* expr, TypePtr type) { if (type) exprTypes[expr] = std::move(type); }
    Type* getType(const Expr* expr) const {
        auto it = exprTypes.find(expr);
        return (it != exprTypes.end()) ? it->second.get() : nullptr;
    }
private:
    std::unordered_map<const Expr*, TypePtr> exprTypes;
};

// A deliberately small, serializable constant representation. Keeping the
// value textual in the semantic symbol table avoids coupling Semantic.hpp to
// the type-checker's evaluator implementation.
struct ConstValue {
    enum class Kind { Int, Float, Bool, String, Null };
    Kind kind = Kind::Null;
    int64_t intValue = 0;
    double floatValue = 0.0;
    bool boolValue = false;
    std::string stringValue;

    std::string toString() const;
};

class TypeChecker {
public:
    TypeChecker(SemanticAnalyzer& semantic, SemErrorSystem& errors);
    bool checkProgram(const Program& program);
    Type* getExprType(const Expr* expr) const { return typeEnv.getType(expr); }

    // Returns the folded value of a successfully evaluated const declaration.
    const ConstValue* getConstValue(const std::string& name) const;

private:
    SemanticAnalyzer& semantic;
    SemErrorSystem& errors;
    TypeEnv typeEnv;
    TypeFactory typeFactory;

    // Lexical type/value environments used by the type-checking pass. The
    // semantic pass owns declaration identity; these stacks model the AST
    // walk independently so TypeChecker does not depend on SemanticAnalyzer's
    // post-pass scope cursor.
    std::vector<std::unordered_map<std::string, TypePtr>> typeScopes;
    std::vector<std::unordered_map<std::string, ConstValue>> constScopes;
    std::unordered_map<std::string, ConstValue> foldedConsts;

    TypePtr inferExprType(const Expr* expr);
    bool checkStmt(const Stmt* stmt);
    bool checkBlock(const Block* block);

    bool isAssignable(Type* source, Type* target);
    bool typesEqual(const Type* a, const Type* b);
    TypePtr typeFromRef(const TypeRef& ref);

    // Supreme type-system checks.
    void checkInterfaceImplementations(const Program& program);
    void checkClassImplementsInterface(const ClassDecl& cls,
                                       const InterfaceDecl& iface,
                                       const std::unordered_map<std::string, const ClassDecl*>& classes,
                                       const std::unordered_map<std::string, const InterfaceDecl*>& interfaces,
                                       const std::unordered_map<std::string, std::vector<const FunctionDecl*>>& extensions);
    void checkMatchExhaustiveness(const MatchExpr& match, Type* subjectType);

    void collectTypeDeclarations(const Block* block,
                                 std::unordered_map<std::string, const InterfaceDecl*>& interfaces,
                                 std::unordered_map<std::string, const ClassDecl*>& classes,
                                 std::unordered_map<std::string, const EnumDecl*>& enums,
                                 std::unordered_map<std::string, std::vector<const FunctionDecl*>>& extensions) const;
    void collectTypeDeclarations(const Program& program,
                                 std::unordered_map<std::string, const InterfaceDecl*>& interfaces,
                                 std::unordered_map<std::string, const ClassDecl*>& classes,
                                 std::unordered_map<std::string, const EnumDecl*>& enums,
                                 std::unordered_map<std::string, std::vector<const FunctionDecl*>>& extensions) const;

    std::vector<std::string> inheritedInterfaces(
        const ClassDecl& cls,
        const std::unordered_map<std::string, const ClassDecl*>& classes,
        const std::unordered_map<std::string, const InterfaceDecl*>& interfaces) const;

    const FunctionDecl* findMethod(
        const ClassDecl& cls,
        const std::string& methodName,
        const std::unordered_map<std::string, const ClassDecl*>& classes,
        const std::unordered_map<std::string, std::vector<const FunctionDecl*>>& extensions,
        std::unordered_set<std::string>& visitedClasses) const;

    static std::string parameterTypeName(const std::string& raw);
    static std::string normalizeTypeName(const std::string& raw);
    static bool parameterTypeCompatible(const std::string& expected,
                                         const std::string& actual);
    static bool patternMatchesEnumValue(const Expr* pattern,
                                         const std::string& enumName,
                                         const std::string& valueName);
    static std::string patternKey(const Expr* pattern);

    // Const evaluator.
    std::optional<ConstValue> evaluateConstExpr(const Expr* expr);
    std::optional<ConstValue> evalBinaryConst(const BinaryExpr& expr);
    bool evalAsBool(const ConstValue& value, bool& out) const;
    bool evalAsNumber(const ConstValue& value, long double& out, bool& isFloat) const;
    std::string constValueText(const ConstValue& value) const;
    void publishConst(const DeclarationStmt& decl, const ConstValue& value);

    TypePtr lookupType(const std::string& name) const;
    const ConstValue* lookupConst(const std::string& name) const;

    void pushScope();
    void popScope();
    void typeError(int line, int col, const std::string& msg);
    void exhaustivenessError(int line, int col, const std::string& msg);
    void interfaceError(int line, int col, const std::string& msg);
    void constError(int line, int col, const std::string& msg);
};

} // namespace nova
