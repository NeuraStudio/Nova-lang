// Type.hpp — Nova Language Supreme Type System
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace nova {

enum class TypeKind {
    Void, Bool, Int, Float, String, Null,
    Array, Tuple, Map, Set, Function,
    Class, Struct, Interface, Enum,
    TypeParam, Any, Optional, Result, Channel, Signal, Alias
};

struct Type {
    TypeKind kind;
    std::string name;
    std::vector<std::unique_ptr<Type>> generics;
    std::vector<std::unique_ptr<Type>> paramTypes;
    std::unique_ptr<Type> returnType;
    std::unique_ptr<Type> elementType;
    std::vector<std::unique_ptr<Type>> elementTypes;
    std::unique_ptr<Type> keyType;
    std::unique_ptr<Type> valueType;
    std::unique_ptr<Type> innerType;
    std::string paramName;
    std::string aliasTargetName;

    Type(TypeKind k = TypeKind::Void) : kind(k) {}

    std::string toString() const {
        switch (kind) {
            case TypeKind::Void: return "Void";
            case TypeKind::Bool: return "Bool";
            case TypeKind::Int: return "Int";
            case TypeKind::Float: return "Float";
            case TypeKind::String: return "String";
            case TypeKind::Null: return "Null";
            case TypeKind::Any: return "Any";
            case TypeKind::Class: case TypeKind::Struct: case TypeKind::Enum: return name.empty() ? "Object" : name;
            case TypeKind::Array: return "Array";
            case TypeKind::Map: return "Map";
            default: return "CustomType";
        }
    }
};

using TypePtr = std::unique_ptr<Type>;

class TypeFactory {
public:
    static TypePtr voidType() { return std::make_unique<Type>(TypeKind::Void); }
    static TypePtr boolType() { return std::make_unique<Type>(TypeKind::Bool); }
    static TypePtr intType() { return std::make_unique<Type>(TypeKind::Int); }
    static TypePtr floatType() { return std::make_unique<Type>(TypeKind::Float); }
    static TypePtr stringType() { return std::make_unique<Type>(TypeKind::String); }
    static TypePtr nullType() { return std::make_unique<Type>(TypeKind::Null); }
    static TypePtr anyType() { return std::make_unique<Type>(TypeKind::Any); }
    static TypePtr arrayType(TypePtr el) { auto t = std::make_unique<Type>(TypeKind::Array); t->elementType = std::move(el); return t; }
    static TypePtr mapType(TypePtr k, TypePtr v) { auto t = std::make_unique<Type>(TypeKind::Map); t->keyType = std::move(k); t->valueType = std::move(v); return t; }
    static TypePtr namedType(const std::string& name) { auto t = std::make_unique<Type>(TypeKind::Class); t->name = name; return t; }
};

} // namespace nova
