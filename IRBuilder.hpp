#pragma once
// Nova AST -> Nova SSA IR lowering.
//
// The builder uses the already-validated AST. It intentionally does not try
// to redo type checking or ownership checking; those are responsibilities of
// the previous compiler maps. Where the AST does not carry enough information
// to recover a concrete type, IR uses Any and preserves the operation through
// an explicit runtime instruction.

#include "AST.hpp"
#include "IR.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nova::ir {

class IRBuilder {
public:
    IRBuilder();

    // The Program remains owned by the caller. The returned Module owns all IR.
    std::unique_ptr<Module> build(const nova::Program& program);

private:
    struct LoopContext {
        BasicBlock* continueTarget = nullptr;
        BasicBlock* breakTarget = nullptr;
    };

    struct FunctionContext {
        Function* function = nullptr;
        BasicBlock* current = nullptr;

        // SSA environment: each source variable denotes its current SSA value.
        std::unordered_map<std::string, ValuePtr> values;

        // Addressable values use an explicit slot. This is needed for member
        // and index assignment and is also useful to later mem2reg promotion.
        std::unordered_map<std::string, ValuePtr> slots;

        std::vector<LoopContext> loops;
        BasicBlock* errorTarget = nullptr;
        ValuePtr errorValue;

        std::size_t lambdaCounter = 0;
        std::size_t tempCounter = 0;
    };

    std::unique_ptr<Module> module_;
    FunctionContext* ctx_ = nullptr;
    std::vector<std::string> genericStack_;
    std::unordered_map<std::string, const FunctionDecl*> genericDecls_;
    ValueId nextValueId_ = 1;

    // ---------- module/function lowering ----------
    void lowerTopLevel(const nova::Program& program);
    void lowerStmt(const Stmt& stmt);
    void lowerBlock(const Block& block);
    void lowerFunction(const FunctionDecl& fn,
                       const std::string& forcedName = {});
    void lowerClass(const ClassDecl& c);
    void lowerModuleDecl(const ModuleDecl& m);
    void lowerAnnotated(const AnnotatedStmt& a);

    // ---------- statements ----------
    void lowerIf(const IfStmt& s);
    void lowerSwitch(const SwitchStmt& s);
    void lowerWhile(const WhileStmt& s);
    void lowerFor(const ForStmt& s);
    void lowerRepeat(const RepeatStmt& s);
    void lowerForeach(const ForeachStmt& s);
    void lowerTry(const TryStmt& s);
    void lowerReturn(const ReturnStmt& s);
    void lowerAssignment(const AssignmentStmt& s);
    void lowerDeclaration(const DeclarationStmt& s);
    void lowerDestructuring(const DestructuringStmt& s);
    void lowerUsing(const UsingStmt& s);
    void lowerGuard(const GuardStmt& s);
    void lowerThread(const ThreadStmt& s);
    void lowerEvent(const EventStmt& s);
    void lowerUnsafe(const UnsafeStmt& s);
    void lowerSignal(const SignalDecl& s);
    void lowerLazy(const LazyDecl& s);
    void lowerComptime(const ComptimeStmt& s);
    void lowerExtend(const ExtendStmt& s);
    void lowerChan(const ChanDecl& s);

    // ---------- expressions ----------
    ValuePtr lowerExpr(const Expr& e);
    ValuePtr lowerLiteral(const LiteralExpr& e);
    ValuePtr lowerIdentifier(const IdentifierExpr& e);
    ValuePtr lowerBinary(const BinaryExpr& e);
    ValuePtr lowerUnary(const UnaryExpr& e);
    ValuePtr lowerCall(const CallExpr& e);
    ValuePtr lowerMember(const MemberExpr& e);
    ValuePtr lowerIndex(const IndexExpr& e);
    ValuePtr lowerSlice(const SliceExpr& e);
    ValuePtr lowerArray(const ArrayLiteralExpr& e);
    ValuePtr lowerMap(const MapLiteralExpr& e);
    ValuePtr lowerTuple(const TupleLiteralExpr& e);
    ValuePtr lowerLambda(const LambdaExpr& e);
    ValuePtr lowerTernary(const TernaryExpr& e);
    ValuePtr lowerElvis(const ElvisExpr& e);
    ValuePtr lowerNullCoalesce(const NullCoalesceExpr& e);
    ValuePtr lowerRange(const RangeExpr& e);
    ValuePtr lowerMatchExpr(const MatchExpr& e);
    ValuePtr lowerCast(const AsCastExpr& e);
    ValuePtr lowerExists(const ExistsExpr& e);
    ValuePtr lowerSpread(const SpreadExpr& e);
    ValuePtr lowerDictComprehension(const DictComprehensionExpr& e);

    // ---------- CFG / SSA ----------
    BasicBlock* makeBlock(const std::string& hint);
    void setCurrent(BasicBlock* bb);
    void branch(BasicBlock* target);
    void condBranch(const ValuePtr& cond, BasicBlock* yes, BasicBlock* no);
    void ensureOpen(BasicBlock* continuationHint = nullptr);

    ValuePtr emit(Opcode op, Type type, std::vector<ValuePtr> operands = {},
                  const std::string& hint = {});
    ValuePtr emitVoid(Opcode op, std::vector<ValuePtr> operands = {},
                      const std::string& hint = {});

    ValuePtr makeConstant(const Type& type, const std::string& text);
    ValuePtr makeUndef(const Type& type);
    ValuePtr makePhi(const Type& type,
                     const std::vector<PhiIncoming>& incoming,
                     const std::string& hint = {});

    // Merge the two SSA environments at an if/else join.
    void mergeEnvironments(BasicBlock* join,
                           const FunctionContext& before,
                           const FunctionContext& thenEnv,
                           const FunctionContext& elseEnv);

    // Loop-carried SSA values. This creates header phis and returns their
    // destination values; incoming values are patched after the loop body.
    ValuePtr createLoopPhi(const std::string& variable,
                           const ValuePtr& initial,
                           BasicBlock* header,
                           BasicBlock* preheader);
    void addPhiIncoming(const ValuePtr& phi,
                        const ValuePtr& value,
                        BasicBlock* predecessor);

    // ---------- source/runtime helpers ----------
    Type typeFromName(const std::string& name) const;
    Type typeFromRef(const TypeRef& ref) const;
    Type inferType(const Expr& e) const;
    std::string mangleGeneric(const FunctionDecl& fn,
                              const std::vector<Type>& argumentTypes) const;
    std::string mangleGenericName(const std::string& base,
                                   const std::vector<Type>& types) const;

    ValuePtr runtimeCall(const std::string& name,
                         const std::vector<ValuePtr>& args,
                         Type result = Type::anyTy());

    // Async lowering is deliberately explicit: await becomes a state-machine
    // suspension point plus runtime hooks. Later passes may lower the hooks to
    // a target-specific coroutine implementation.
    ValuePtr lowerAwaitCall(const CallExpr& call);

    // ! / error propagation is represented as an ErrorCheck + conditional CFG.
    ValuePtr lowerErrorPropagation(const ValuePtr& result,
                                   const std::string& sourceName);

    void emitReturnIfNeeded();
    std::string uniqueName(const std::string& prefix);
};

} // namespace nova::ir
