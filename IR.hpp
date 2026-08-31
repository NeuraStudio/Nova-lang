#pragma once
// Nova IR — SSA-oriented intermediate representation.
// C++17, deliberately independent from the parser/semantic-analyzer
// implementation except for the small amount of type information represented
// as strings. The IR is therefore a stable boundary between the frontend and
// later optimization/code-generation passes.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nova::ir {

using ValueId = std::uint64_t;

enum class TypeKind {
    Void, Bool, Int, Float, String, Null, Any, Pointer, Aggregate, Function
};

struct Type {
    TypeKind kind = TypeKind::Any;
    std::string name;
    std::vector<Type> parameters;

    static Type voidTy()   { return {TypeKind::Void, "Void", {}}; }
    static Type boolTy()   { return {TypeKind::Bool, "Bool", {}}; }
    static Type intTy()    { return {TypeKind::Int, "Int", {}}; }
    static Type floatTy()  { return {TypeKind::Float, "Float", {}}; }
    static Type stringTy() { return {TypeKind::String, "String", {}}; }
    static Type nullTy()   { return {TypeKind::Null, "Null", {}}; }
    static Type anyTy()    { return {TypeKind::Any, "Any", {}}; }

    std::string str() const;
    bool operator==(const Type& o) const;
    bool operator!=(const Type& o) const { return !(*this == o); }
};

enum class ValueKind {
    Argument, Constant, Instruction, Global, Undef
};

struct Value {
    ValueId id = 0;
    Type type;
    std::string name;
    ValueKind kind = ValueKind::Undef;

    Value() = default;
    Value(Type t, std::string n = {}, ValueKind k = ValueKind::Undef)
        : type(std::move(t)), name(std::move(n)), kind(k) {}
    virtual ~Value() = default;

    std::string ref() const;
};

using ValuePtr = std::shared_ptr<Value>;

struct ConstantValue final : Value {
    std::string literal;
    ConstantValue(Type t, std::string text)
        : Value(std::move(t), {}, ValueKind::Constant), literal(std::move(text)) {}
};

enum class Opcode {
    Nop,
    Alloc, Load, Store,
    Add, Sub, Mul, Div, Mod, Pow,
    Neg, Not,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
    Call,
    Jump, CondBranch, Return,
    Phi,
    Select,
    Cast,
    Index, Member,
    MakeArray, MakeMap, MakeTuple,
    Slice,
    Invoke,        // call with explicit exceptional successor
    Await,
    Yield,
    AsyncSuspend,
    AsyncResume,
    ErrorCheck,
    Runtime
};

struct BasicBlock;

struct Instruction final : Value {
    Opcode opcode = Opcode::Nop;
    std::vector<ValuePtr> operands;
    std::vector<BasicBlock*> successors;
    std::unordered_map<std::string, std::string> attributes;

    Instruction(Type t, Opcode op, std::string n = {})
        : Value(std::move(t), std::move(n), ValueKind::Instruction), opcode(op) {}

    bool isTerminator() const;
};

using InstructionPtr = std::shared_ptr<Instruction>;

struct PhiIncoming {
    ValuePtr value;
    BasicBlock* predecessor = nullptr;
};

struct BasicBlock {
    std::uint64_t id = 0;
    std::string name;
    std::vector<InstructionPtr> instructions;
    std::vector<BasicBlock*> predecessors;
    std::vector<BasicBlock*> successors;

    explicit BasicBlock(std::uint64_t i = 0, std::string n = {})
        : id(i), name(std::move(n)) {}

    InstructionPtr terminator() const;
    bool terminated() const { return static_cast<bool>(terminator()); }
};

struct Function {
    std::string name;
    Type returnType = Type::anyTy();
    std::vector<ValuePtr> arguments;
    std::vector<std::unique_ptr<BasicBlock>> blocks;
    std::unordered_map<std::string, std::string> attributes;
    bool isAsync = false;
    bool isGeneric = false;
    std::vector<std::string> genericParameters;

    BasicBlock* entry() const;
    BasicBlock* createBlock(const std::string& blockName);
};

struct Module {
    std::string name;
    std::vector<std::unique_ptr<Function>> functions;
    std::unordered_map<std::string, ValuePtr> globals;
    std::vector<std::string> imports;
    std::vector<std::string> exports;

    Function* createFunction(const std::string& functionName,
                             Type returnType = Type::anyTy());
    Function* findFunction(const std::string& functionName) const;
};

// Textual form is intentionally compact and deterministic. It is useful for
// golden tests and for debugging the frontend/IR boundary.
std::string print(const Module& module);

} // namespace nova::ir
