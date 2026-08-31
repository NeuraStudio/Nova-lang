// TypeChecker.cpp — Nova Supreme Type Checker
#include "TypeChecker.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace nova {
namespace {

std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

bool isWildcard(const Expr* e) {
    const auto* id = dynamic_cast<const IdentifierExpr*>(e);
    return id && id->name == "_";
}

} // namespace

std::string ConstValue::toString() const {
    std::ostringstream os;
    switch (kind) {
        case Kind::Int: return std::to_string(intValue);
        case Kind::Float:
            os << std::setprecision(17) << floatValue;
            return os.str();
        case Kind::Bool: return boolValue ? "true" : "false";
        case Kind::String: return stringValue;
        case Kind::Null: return "null";
    }
    return {};
}

TypeChecker::TypeChecker(SemanticAnalyzer& semantic, SemErrorSystem& errors)
    : semantic(semantic), errors(errors) {
    pushScope();
}

void TypeChecker::pushScope() {
    typeScopes.emplace_back();
    constScopes.emplace_back();
}

void TypeChecker::popScope() {
    if (typeScopes.size() > 1) {
        typeScopes.pop_back();
        constScopes.pop_back();
    }
}

TypePtr TypeChecker::lookupType(const std::string& name) const {
    for (auto it = typeScopes.rbegin(); it != typeScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found == it->end() || !found->second) continue;
        std::function<TypePtr(const Type&)> clone = [&](const Type& t) -> TypePtr {
            auto out = std::make_unique<Type>(t.kind);
            out->name = t.name;
            out->paramName = t.paramName;
            out->aliasTargetName = t.aliasTargetName;
            for (const auto& x : t.generics) out->generics.push_back(clone(*x));
            for (const auto& x : t.paramTypes) out->paramTypes.push_back(clone(*x));
            if (t.returnType) out->returnType = clone(*t.returnType);
            if (t.elementType) out->elementType = clone(*t.elementType);
            for (const auto& x : t.elementTypes) out->elementTypes.push_back(clone(*x));
            if (t.keyType) out->keyType = clone(*t.keyType);
            if (t.valueType) out->valueType = clone(*t.valueType);
            if (t.innerType) out->innerType = clone(*t.innerType);
            return out;
        };
        return clone(*found->second);
    }
    return nullptr;
}

const ConstValue* TypeChecker::lookupConst(const std::string& name) const {
    for (auto it = constScopes.rbegin(); it != constScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

const ConstValue* TypeChecker::getConstValue(const std::string& name) const {
    auto it = foldedConsts.find(name);
    return it == foldedConsts.end() ? nullptr : &it->second;
}

TypePtr TypeChecker::typeFromRef(const TypeRef& ref) {
    if (ref.name == "Void") return typeFactory.voidType();
    if (ref.name == "Bool") return typeFactory.boolType();
    if (ref.name == "Int") return typeFactory.intType();
    if (ref.name == "Float") return typeFactory.floatType();
    if (ref.name == "String") return typeFactory.stringType();
    if (ref.name == "Null") return typeFactory.nullType();
    if (ref.name == "Any") return typeFactory.anyType();
    if (ref.name == "Array") return typeFactory.arrayType(ref.generics.empty() ? typeFactory.anyType() : typeFromRef(ref.generics.front()));
    if (ref.name == "Map") {
        auto k = ref.generics.empty() ? typeFactory.anyType() : typeFromRef(ref.generics[0]);
        auto v = ref.generics.size() < 2 ? typeFactory.anyType() : typeFromRef(ref.generics[1]);
        return typeFactory.mapType(std::move(k), std::move(v));
    }
    if (ref.name == "Set") {
        auto t = std::make_unique<Type>(TypeKind::Set);
        t->elementType = ref.generics.empty() ? typeFactory.anyType() : typeFromRef(ref.generics.front());
        return t;
    }
    if (ref.name == "Optional") {
        auto t = std::make_unique<Type>(TypeKind::Optional);
        t->innerType = ref.generics.empty() ? typeFactory.anyType() : typeFromRef(ref.generics.front());
        return t;
    }
    if (ref.name == "Result") {
        auto t = std::make_unique<Type>(TypeKind::Result);
        for (const auto& g : ref.generics) t->generics.push_back(typeFromRef(g));
        return t;
    }
    if (ref.name == "Channel") {
        auto t = std::make_unique<Type>(TypeKind::Channel);
        t->elementType = ref.generics.empty() ? typeFactory.anyType() : typeFromRef(ref.generics.front());
        return t;
    }
    for (const Symbol& sym : semantic.symbols().allSymbols()) {
        if (sym.name != ref.name) continue;
        TypeKind kind;
        switch (sym.kind) {
            case SymbolKind::Interface: kind = TypeKind::Interface; break;
            case SymbolKind::Enum: kind = TypeKind::Enum; break;
            case SymbolKind::Struct: kind = TypeKind::Struct; break;
            case SymbolKind::Class: kind = TypeKind::Class; break;
            case SymbolKind::TypeAlias: {
                auto t = std::make_unique<Type>(TypeKind::Alias);
                t->name = ref.name; t->aliasTargetName = sym.typeName; return t;
            }
            default: continue;
        }
        auto t = std::make_unique<Type>(kind); t->name = ref.name; return t;
    }
    return typeFactory.namedType(ref.name);
}

TypePtr TypeChecker::inferExprType(const Expr* expr) {
    if (!expr) return typeFactory.voidType();

    if (auto* lit = dynamic_cast<const LiteralExpr*>(expr)) {
        TypePtr t;
        switch (lit->kind) {
            case LiteralKind::Int: t = typeFactory.intType(); break;
            case LiteralKind::Float: t = typeFactory.floatType(); break;
            case LiteralKind::String:
            case LiteralKind::RawString: t = typeFactory.stringType(); break;
            case LiteralKind::Bool: t = typeFactory.boolType(); break;
            case LiteralKind::Null: t = typeFactory.nullType(); break;
        }
        typeEnv.setType(expr, std::move(t));
        const Type* stored = typeEnv.getType(expr);
        if (!stored) return typeFactory.anyType();
        auto copy = std::make_unique<Type>(stored->kind);
        copy->name = stored->name;
        return copy;
    }

    if (auto* id = dynamic_cast<const IdentifierExpr*>(expr)) {
        if (const ConstValue* cv = lookupConst(id->name)) {
            switch (cv->kind) {
                case ConstValue::Kind::Int: return typeFactory.intType();
                case ConstValue::Kind::Float: return typeFactory.floatType();
                case ConstValue::Kind::Bool: return typeFactory.boolType();
                case ConstValue::Kind::String: return typeFactory.stringType();
                case ConstValue::Kind::Null: return typeFactory.nullType();
            }
        }
        if (TypePtr t = lookupType(id->name)) return t;
        for (const Symbol& sym : semantic.symbols().allSymbols()) {
            if (sym.name != id->name) continue;
            if (sym.kind == SymbolKind::EnumValue && !sym.typeName.empty()) {
                auto t = std::make_unique<Type>(TypeKind::Enum); t->name = sym.typeName; return t;
            }
            if (sym.kind == SymbolKind::Interface || sym.kind == SymbolKind::Enum ||
                sym.kind == SymbolKind::Struct || sym.kind == SymbolKind::Class) {
                auto t = std::make_unique<Type>(sym.kind == SymbolKind::Interface ? TypeKind::Interface :
                                                sym.kind == SymbolKind::Enum ? TypeKind::Enum :
                                                sym.kind == SymbolKind::Struct ? TypeKind::Struct : TypeKind::Class);
                t->name = id->name; return t;
            }
        }
        return typeFactory.anyType();
    }

    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        TypePtr l = inferExprType(bin->left.get());
        TypePtr r = inferExprType(bin->right.get());
        const std::string& op = bin->op;
        if (op == "&&" || op == "||" || op == "==" || op == "!=" ||
            op == "<" || op == "<=" || op == ">" || op == ">=") return typeFactory.boolType();
        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%" || op == "^") {
            if (op == "+" && (l->kind == TypeKind::String || r->kind == TypeKind::String)) return typeFactory.stringType();
            if (l->kind == TypeKind::Float || r->kind == TypeKind::Float) return typeFactory.floatType();
            if (l->kind == TypeKind::Int && r->kind == TypeKind::Int) return typeFactory.intType();
            return typeFactory.anyType();
        }
        return typeFactory.anyType();
    }

    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        TypePtr operand = inferExprType(un->operand.get());
        if (un->op == "!") return typeFactory.boolType();
        if (un->op == "-" || un->op == "++" || un->op == "--") return operand;
        return operand;
    }

    if (auto* tern = dynamic_cast<const TernaryExpr*>(expr)) {
        TypePtr a = inferExprType(tern->thenExpr.get());
        TypePtr b = inferExprType(tern->elseExpr.get());
        return typesEqual(a.get(), b.get()) ? std::move(a) : typeFactory.anyType();
    }
    if (auto* elvis = dynamic_cast<const ElvisExpr*>(expr)) {
        TypePtr a = inferExprType(elvis->left.get());
        TypePtr b = inferExprType(elvis->fallback.get());
        return typesEqual(a.get(), b.get()) ? std::move(a) : typeFactory.anyType();
    }
    if (auto* co = dynamic_cast<const NullCoalesceExpr*>(expr)) {
        TypePtr a = inferExprType(co->left.get());
        TypePtr b = inferExprType(co->fallback.get());
        return typesEqual(a.get(), b.get()) ? std::move(a) : typeFactory.anyType();
    }
    if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
        inferExprType(call->callee.get());
        for (const auto& arg : call->args) inferExprType(arg.get());
        return typeFactory.anyType();
    }
    if (auto* arr = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
        if (arr->elements.empty()) return typeFactory.arrayType(typeFactory.anyType());
        TypePtr first = inferExprType(arr->elements.front().get());
        for (size_t i = 1; i < arr->elements.size(); ++i) {
            TypePtr next = inferExprType(arr->elements[i].get());
            if (!typesEqual(first.get(), next.get())) return typeFactory.arrayType(typeFactory.anyType());
        }
        return typeFactory.arrayType(std::move(first));
    }
    if (auto* tuple = dynamic_cast<const TupleLiteralExpr*>(expr)) {
        auto t = std::make_unique<Type>(TypeKind::Tuple);
        for (const auto& e : tuple->elements) t->elementTypes.push_back(inferExprType(e.get()));
        return t;
    }
    if (auto* map = dynamic_cast<const MapLiteralExpr*>(expr)) {
        if (map->entries.empty()) return typeFactory.mapType(typeFactory.anyType(), typeFactory.anyType());
        return typeFactory.mapType(inferExprType(map->entries.front().key.get()), inferExprType(map->entries.front().value.get()));
    }
    if (auto* match = dynamic_cast<const MatchExpr*>(expr)) {
        TypePtr subject = inferExprType(match->subject.get());
        checkMatchExhaustiveness(*match, subject.get());
        return typeFactory.anyType();
    }
    if (auto* cast = dynamic_cast<const AsCastExpr*>(expr)) {
        inferExprType(cast->target.get());
        return typeFromRef(TypeRef{cast->typeName, {}});
    }
    if (auto* member = dynamic_cast<const MemberExpr*>(expr)) {
        TypePtr target = inferExprType(member->target.get());
        if (target && target->kind == TypeKind::Enum && !target->name.empty()) {
            for (const Symbol& sym : semantic.symbols().allSymbols()) {
                if (sym.kind == SymbolKind::Enum && sym.name == target->name &&
                    std::find(sym.memberNames.begin(), sym.memberNames.end(), member->name) != sym.memberNames.end()) {
                    auto out = std::make_unique<Type>(TypeKind::Enum); out->name = target->name; return out;
                }
            }
        }
        return typeFactory.anyType();
    }
    if (auto* index = dynamic_cast<const IndexExpr*>(expr)) {
        TypePtr target = inferExprType(index->target.get());
        inferExprType(index->index.get());
        return target && target->kind == TypeKind::Array && target->elementType
                   ? std::move(target->elementType) : typeFactory.anyType();
    }
    if (auto* slice = dynamic_cast<const SliceExpr*>(expr)) {
        TypePtr target = inferExprType(slice->target.get());
        if (slice->start) inferExprType(slice->start.get());
        if (slice->end) inferExprType(slice->end.get());
        if (slice->step) inferExprType(slice->step.get());
        return target && target->kind == TypeKind::Array ? std::move(target) : typeFactory.anyType();
    }

    return typeFactory.anyType();
}

bool TypeChecker::checkProgram(const Program& program) {
    typeScopes.clear(); constScopes.clear(); foldedConsts.clear(); pushScope();
    bool ok = true;
    for (const auto& stmt : program.statements) ok = checkStmt(stmt.get()) && ok;
    checkInterfaceImplementations(program);
    return ok && !errors.hasErrors();
}

bool TypeChecker::checkBlock(const Block* block) {
    if (!block) return true;
    pushScope(); bool ok = true;
    for (const auto& stmt : block->statements) ok = checkStmt(stmt.get()) && ok;
    popScope(); return ok;
}

bool TypeChecker::checkStmt(const Stmt* stmt) {
    if (!stmt) return true;
    if (auto* decl = dynamic_cast<const DeclarationStmt*>(stmt)) {
        TypePtr actual = decl->value ? inferExprType(decl->value.get()) : typeFactory.anyType();
        if (decl->typeAnnotation) {
            TypePtr expected = typeFromRef(*decl->typeAnnotation);
            if (!isAssignable(actual.get(), expected.get()))
                typeError(decl->line, decl->col, "cannot initialize '" + decl->name + "' of type " + expected->toString() + " with " + actual->toString());
            actual = std::move(expected);
        }
        typeScopes.back()[decl->name] = std::move(actual);
        if (decl->isConst) {
            if (!decl->value) constError(decl->line, decl->col, "const '" + decl->name + "' must have a compile-time initializer");
            else if (auto folded = evaluateConstExpr(decl->value.get())) publishConst(*decl, *folded);
            else constError(decl->line, decl->col, "const '" + decl->name + "' must be initialized by a compile-time expression");
        }
        return true;
    }
    if (auto* asg = dynamic_cast<const AssignmentStmt*>(stmt)) {
        TypePtr rhs = inferExprType(asg->value.get());
        if (auto* id = dynamic_cast<const IdentifierExpr*>(asg->target.get())) {
            TypePtr lhs = lookupType(id->name);
            if (lhs && !isAssignable(rhs.get(), lhs.get()))
                typeError(asg->line, asg->col, "cannot assign " + rhs->toString() + " to '" + id->name + "' of type " + lhs->toString());
            if (lookupConst(id->name)) typeError(asg->line, asg->col, "cannot assign to const '" + id->name + "'");
        }
        inferExprType(asg->target.get()); return true;
    }
    if (auto* e = dynamic_cast<const ExprStmt*>(stmt)) { inferExprType(e->expr.get()); return true; }
    if (auto* r = dynamic_cast<const ReturnStmt*>(stmt)) { if (r->value) inferExprType(r->value.get()); return true; }
    if (auto* i = dynamic_cast<const IfStmt*>(stmt)) {
        TypePtr c = inferExprType(i->condition.get());
        if (c->kind != TypeKind::Bool && c->kind != TypeKind::Any) typeError(i->line, i->col, "if condition must be Bool");
        checkBlock(i->thenBranch.get()); if (i->elseIf) checkStmt(i->elseIf.get()); checkBlock(i->elseBranch.get()); return true;
    }
    if (auto* w = dynamic_cast<const WhileStmt*>(stmt)) { inferExprType(w->condition.get()); checkBlock(w->body.get()); return true; }
    if (auto* r = dynamic_cast<const RepeatStmt*>(stmt)) { inferExprType(r->count.get()); checkBlock(r->body.get()); return true; }
    if (auto* f = dynamic_cast<const ForStmt*>(stmt)) {
        inferExprType(f->from.get()); inferExprType(f->to.get()); if (f->step) inferExprType(f->step.get());
        pushScope(); typeScopes.back()[f->varName] = typeFactory.intType(); checkBlock(f->body.get()); popScope(); return true;
    }
    if (auto* f = dynamic_cast<const ForeachStmt*>(stmt)) {
        TypePtr t = inferExprType(f->iterable.get()); pushScope();
        typeScopes.back()[f->varName] = (t->kind == TypeKind::Array && t->elementType) ? std::move(t->elementType) : typeFactory.anyType();
        checkBlock(f->body.get()); popScope(); return true;
    }
    if (auto* f = dynamic_cast<const FunctionDecl*>(stmt)) {
        pushScope(); for (const auto& p : f->params) typeScopes.back()[p.name] = typeFactory.anyType();
        checkBlock(f->body.get()); popScope(); return true;
    }
    if (auto* c = dynamic_cast<const ClassDecl*>(stmt)) { for (const auto& m : c->members) if (m.isMethod && m.method) checkStmt(m.method.get()); return true; }
    if (auto* m = dynamic_cast<const ModuleDecl*>(stmt)) { checkBlock(m->body.get()); return true; }
    if (auto* t = dynamic_cast<const TryStmt*>(stmt)) { checkBlock(t->tryBlock.get()); checkBlock(t->catchBlock.get()); checkBlock(t->finallyBlock.get()); return true; }
    if (auto* u = dynamic_cast<const UnsafeStmt*>(stmt)) { checkBlock(u->body.get()); return true; }
    if (auto* c = dynamic_cast<const ComptimeStmt*>(stmt)) { checkBlock(c->body.get()); return true; }
    if (auto* a = dynamic_cast<const AnnotatedStmt*>(stmt)) return checkStmt(a->inner.get());
    if (auto* x = dynamic_cast<const ExtendStmt*>(stmt)) { for (const auto& m : x->methods) if (m) checkStmt(m.get()); return true; }
    if (auto* sw = dynamic_cast<const SwitchStmt*>(stmt)) {
        inferExprType(sw->subject.get()); for (const auto& c : sw->cases) { inferExprType(c.value.get()); for (const auto& st : c.body) checkStmt(st.get()); }
        for (const auto& st : sw->defaultBody) checkStmt(st.get());
        return true;
    }
    return true;
}

bool TypeChecker::isAssignable(Type* source, Type* target) {
    if (!source || !target) return false;
    if (source->kind == TypeKind::Any || target->kind == TypeKind::Any) return true;
    if (source->kind == target->kind && source->name == target->name) return true;
    if (target->kind == TypeKind::Interface && source->kind == TypeKind::Class) {
        std::string current = source->name; std::unordered_set<std::string> seen;
        while (!current.empty() && seen.insert(current).second) {
            bool found = false;
            for (const Symbol& sym : semantic.symbols().allSymbols()) if (sym.name == current && sym.kind == SymbolKind::Class) {
                found = true; if (sym.baseClassName == target->name) return true; current = sym.baseClassName; break;
            }
            if (!found) break;
            if (current == target->name) return true;
        }
    }
    return false;
}

bool TypeChecker::typesEqual(const Type* a, const Type* b) {
    if (!a || !b || a->kind != b->kind || a->name != b->name) return false;
    if (a->kind == TypeKind::Array || a->kind == TypeKind::Set || a->kind == TypeKind::Channel) return typesEqual(a->elementType.get(), b->elementType.get());
    if (a->kind == TypeKind::Map) return typesEqual(a->keyType.get(), b->keyType.get()) && typesEqual(a->valueType.get(), b->valueType.get());
    if (a->kind == TypeKind::Optional) return typesEqual(a->innerType.get(), b->innerType.get());
    if (a->kind == TypeKind::Tuple) { if (a->elementTypes.size() != b->elementTypes.size()) return false; for (size_t i=0;i<a->elementTypes.size();++i) if (!typesEqual(a->elementTypes[i].get(), b->elementTypes[i].get())) return false; }
    return true;
}

void TypeChecker::typeError(int line, int col, const std::string& msg) { errors.error(line, col, "E3001", msg); }
void TypeChecker::exhaustivenessError(int line, int col, const std::string& msg) { errors.error(line, col, "E3002", msg); }
void TypeChecker::interfaceError(int line, int col, const std::string& msg) { errors.error(line, col, "E3003", msg); }
void TypeChecker::constError(int line, int col, const std::string& msg) { errors.error(line, col, "E3004", msg); }

std::string TypeChecker::normalizeTypeName(const std::string& raw) {
    std::string out = trim(raw); const auto colon = out.find(':'); if (colon != std::string::npos) out = trim(out.substr(colon + 1));
    while (!out.empty() && (out.back() == ',' || out.back() == ';')) out.pop_back();
    return trim(out);
}

std::string TypeChecker::parameterTypeName(const std::string& raw) {
    const std::string s = trim(raw); if (s.empty()) return {};
    const auto colon = s.find(':'); if (colon != std::string::npos) return normalizeTypeName(s.substr(colon + 1));
    std::istringstream iss(s); std::string first, second; iss >> first >> second; if (!second.empty()) return normalizeTypeName(first);
    static const std::unordered_set<std::string> known = {"Void","Bool","Int","Float","String","Null","Any","Array","Map","Set","Optional","Result","Channel","Signal"};
    return known.count(first) ? first : std::string{};
}

bool TypeChecker::parameterTypeCompatible(const std::string& expected, const std::string& actual) {
    const std::string e = normalizeTypeName(expected), a = normalizeTypeName(actual); return e.empty() || a.empty() || e == a;
}

void TypeChecker::collectTypeDeclarations(const Block* block, std::unordered_map<std::string,const InterfaceDecl*>& interfaces, std::unordered_map<std::string,const ClassDecl*>& classes, std::unordered_map<std::string,const EnumDecl*>& enums, std::unordered_map<std::string,std::vector<const FunctionDecl*>>& extensions) const {
    if (!block) return;
    for (const auto& st : block->statements) {
        if (!st) continue;
        if (auto* i=dynamic_cast<const InterfaceDecl*>(st.get())) interfaces[i->name]=i;
        else if (auto* c=dynamic_cast<const ClassDecl*>(st.get())) classes[c->name]=c;
        else if (auto* e=dynamic_cast<const EnumDecl*>(st.get())) enums[e->name]=e;
        else if (auto* x=dynamic_cast<const ExtendStmt*>(st.get())) {
            for (const auto& m : x->methods) {
                if (m) extensions[x->typeName].push_back(m.get());
            }
        }
        else if (auto* m=dynamic_cast<const ModuleDecl*>(st.get())) collectTypeDeclarations(m->body.get(),interfaces,classes,enums,extensions);
        else if (auto* a=dynamic_cast<const AnnotatedStmt*>(st.get())) {
            if (auto* i=dynamic_cast<const InterfaceDecl*>(a->inner.get())) interfaces[i->name]=i;
            else if (auto* c=dynamic_cast<const ClassDecl*>(a->inner.get())) classes[c->name]=c;
            else if (auto* e=dynamic_cast<const EnumDecl*>(a->inner.get())) enums[e->name]=e;
        }
    }
}

void TypeChecker::collectTypeDeclarations(const Program& program, std::unordered_map<std::string,const InterfaceDecl*>& interfaces, std::unordered_map<std::string,const ClassDecl*>& classes, std::unordered_map<std::string,const EnumDecl*>& enums, std::unordered_map<std::string,std::vector<const FunctionDecl*>>& extensions) const {
    for (const auto& st : program.statements) {
        if (!st) continue;
        if (auto* i=dynamic_cast<const InterfaceDecl*>(st.get())) interfaces[i->name]=i;
        else if (auto* c=dynamic_cast<const ClassDecl*>(st.get())) classes[c->name]=c;
        else if (auto* e=dynamic_cast<const EnumDecl*>(st.get())) enums[e->name]=e;
        else if (auto* x=dynamic_cast<const ExtendStmt*>(st.get())) {
            for (const auto& m : x->methods) {
                if (m) extensions[x->typeName].push_back(m.get());
            }
        }
        else if (auto* m=dynamic_cast<const ModuleDecl*>(st.get())) collectTypeDeclarations(m->body.get(),interfaces,classes,enums,extensions);
        else if (auto* a=dynamic_cast<const AnnotatedStmt*>(st.get())) {
            if (auto* i=dynamic_cast<const InterfaceDecl*>(a->inner.get())) interfaces[i->name]=i;
            else if (auto* c=dynamic_cast<const ClassDecl*>(a->inner.get())) classes[c->name]=c;
            else if (auto* e=dynamic_cast<const EnumDecl*>(a->inner.get())) enums[e->name]=e;
        }
    }
}

std::vector<std::string> TypeChecker::inheritedInterfaces(const ClassDecl& cls, const std::unordered_map<std::string,const ClassDecl*>& classes, const std::unordered_map<std::string,const InterfaceDecl*>& interfaces) const {
    std::vector<std::string> out; std::unordered_set<std::string> seenC, seenI;
    std::function<void(const ClassDecl&)> visit=[&](const ClassDecl& c){ if(!seenC.insert(c.name).second || c.baseName.empty()) return; if(interfaces.count(c.baseName)){ if(seenI.insert(c.baseName).second) out.push_back(c.baseName); return; } auto it=classes.find(c.baseName); if(it!=classes.end()) visit(*it->second); };
    visit(cls); return out;
}

const FunctionDecl* TypeChecker::findMethod(const ClassDecl& cls, const std::string& methodName, const std::unordered_map<std::string,const ClassDecl*>& classes, const std::unordered_map<std::string,std::vector<const FunctionDecl*>>& extensions, std::unordered_set<std::string>& visitedClasses) const {
    if(!visitedClasses.insert(cls.name).second) return nullptr;
    for(const auto& m:cls.members) if(m.isMethod && m.method && m.method->name==methodName) return m.method.get();
    auto ex=extensions.find(cls.name); if(ex!=extensions.end()) for(auto* m:ex->second) if(m && m->name==methodName) return m;
    if(!cls.baseName.empty()){ auto b=classes.find(cls.baseName); if(b!=classes.end()) return findMethod(*b->second,methodName,classes,extensions,visitedClasses); }
    return nullptr;
}

void TypeChecker::checkInterfaceImplementations(const Program& program) {
    std::unordered_map<std::string,const InterfaceDecl*> interfaces; std::unordered_map<std::string,const ClassDecl*> classes; std::unordered_map<std::string,const EnumDecl*> enums; std::unordered_map<std::string,std::vector<const FunctionDecl*>> extensions;
    collectTypeDeclarations(program,interfaces,classes,enums,extensions);
    for(const auto& [name,cls]:classes) for(const auto& in:inheritedInterfaces(*cls,classes,interfaces)) { auto it=interfaces.find(in); if(it!=interfaces.end()) checkClassImplementsInterface(*cls,*it->second,classes,interfaces,extensions); }
}

void TypeChecker::checkClassImplementsInterface(const ClassDecl& cls, const InterfaceDecl& iface, const std::unordered_map<std::string,const ClassDecl*>& classes, const std::unordered_map<std::string,const InterfaceDecl*>& interfaces, const std::unordered_map<std::string,std::vector<const FunctionDecl*>>& extensions) {
    (void)interfaces;
    for(const auto& req:iface.methods){ std::unordered_set<std::string> visited; const FunctionDecl* impl=findMethod(cls,req.name,classes,extensions,visited);
        if(!impl){ interfaceError(cls.line,cls.col,"class '"+cls.name+"' does not implement interface method '"+req.name+"' from '"+iface.name+"'"); continue; }
        if(impl->params.size()!=req.params.size()){ interfaceError(impl->line,impl->col,"method '"+cls.name+"."+req.name+"' has "+std::to_string(impl->params.size())+" parameter(s), but interface '"+iface.name+"' requires "+std::to_string(req.params.size())); continue; }
        for(size_t i=0;i<req.params.size();++i){ const auto e=parameterTypeName(req.params[i]), a=parameterTypeName(impl->params[i].name); if(!parameterTypeCompatible(e,a)) interfaceError(impl->line,impl->col,"parameter "+std::to_string(i+1)+" of '"+cls.name+"."+req.name+"' has type '"+a+"', expected '"+e+"'"); }
    }
}

bool TypeChecker::patternMatchesEnumValue(const Expr* p,const std::string& enumName,const std::string& valueName){
    if(auto* id=dynamic_cast<const IdentifierExpr*>(p)) return id->name==valueName;
    if(auto* m=dynamic_cast<const MemberExpr*>(p)){ auto* owner=dynamic_cast<const IdentifierExpr*>(m->target.get()); return owner && owner->name==enumName && m->name==valueName; }
    return false;
}

std::string TypeChecker::patternKey(const Expr* p){
    if(!p) return "<null>";
    if(auto* id=dynamic_cast<const IdentifierExpr*>(p)) return "id:"+id->name;
    if(auto* m=dynamic_cast<const MemberExpr*>(p)){ auto* o=dynamic_cast<const IdentifierExpr*>(m->target.get()); if(o) return "member:"+o->name+"."+m->name; }
    if(auto* l=dynamic_cast<const LiteralExpr*>(p)){ switch(l->kind){ case LiteralKind::Bool:return std::string("bool:")+(l->boolValue?"true":"false"); case LiteralKind::Int:return "int:"+std::to_string(l->intValue); case LiteralKind::Float:return "float:"+std::to_string(l->floatValue); case LiteralKind::String:case LiteralKind::RawString:return "string:"+l->stringValue; case LiteralKind::Null:return "null"; } }
    return {};
}

void TypeChecker::checkMatchExhaustiveness(const MatchExpr& match, Type* subjectType){
    if(!subjectType || subjectType->kind==TypeKind::Any) return;
    bool wildcard=false, hasTrue=false, hasFalse=false, duplicate=false; std::unordered_set<std::string> seen;
    for(const auto& c:match.cases){ if(!c.pattern){ exhaustivenessError(match.line,match.col,"match case is missing a pattern"); continue; } if(isWildcard(c.pattern.get())){ if(wildcard) duplicate=true; wildcard=true; continue; } auto key=patternKey(c.pattern.get()); if(!key.empty()&&!seen.insert(key).second) duplicate=true; if(auto* l=dynamic_cast<const LiteralExpr*>(c.pattern.get())) if(l->kind==LiteralKind::Bool) { if(l->boolValue) hasTrue=true; else hasFalse=true; } }
    if(duplicate) exhaustivenessError(match.line,match.col,"match contains a duplicate pattern");
    if(match.defaultBody || wildcard) return;
    if(subjectType->kind==TypeKind::Bool){ if(!hasTrue||!hasFalse) exhaustivenessError(match.line,match.col,"non-exhaustive match expression: Bool requires both true and false cases (or default)"); return; }
    if(subjectType->kind==TypeKind::Enum && !subjectType->name.empty()){
        const Symbol* e=nullptr; for(const Symbol& sym:semantic.symbols().allSymbols()) if(sym.kind==SymbolKind::Enum&&sym.name==subjectType->name){e=&sym;break;} if(!e) return;
        std::vector<std::string> missing; for(const auto& value:e->memberNames){ bool covered=false; for(const auto& c:match.cases) if(patternMatchesEnumValue(c.pattern.get(),subjectType->name,value)){covered=true;break;} if(!covered) missing.push_back(value); }
        if(!missing.empty()){ std::ostringstream msg; msg<<"non-exhaustive match expression for enum '"<<subjectType->name<<"': missing "; for(size_t i=0;i<missing.size();++i){if(i)msg<<", ";msg<<missing[i];} msg<<" (or add default)"; exhaustivenessError(match.line,match.col,msg.str()); }
    }
}

std::optional<ConstValue> TypeChecker::evaluateConstExpr(const Expr* expr){
    if(!expr) return std::nullopt;
    if(auto* l=dynamic_cast<const LiteralExpr*>(expr)){ ConstValue v; switch(l->kind){ case LiteralKind::Int:v.kind=ConstValue::Kind::Int;v.intValue=l->intValue;return v; case LiteralKind::Float:v.kind=ConstValue::Kind::Float;v.floatValue=l->floatValue;return v; case LiteralKind::Bool:v.kind=ConstValue::Kind::Bool;v.boolValue=l->boolValue;return v; case LiteralKind::String:case LiteralKind::RawString:v.kind=ConstValue::Kind::String;v.stringValue=l->stringValue;return v; case LiteralKind::Null:v.kind=ConstValue::Kind::Null;return v; } }
    if(auto* id=dynamic_cast<const IdentifierExpr*>(expr)) { if(auto* v=lookupConst(id->name)) return *v; return std::nullopt; }
    if(auto* u=dynamic_cast<const UnaryExpr*>(expr)){ auto v=evaluateConstExpr(u->operand.get()); if(!v)return std::nullopt; if(u->op=="!"){bool b;if(!evalAsBool(*v,b))return std::nullopt;v->kind=ConstValue::Kind::Bool;v->boolValue=!b;return v;} if(u->op=="+"&&(v->kind==ConstValue::Kind::Int||v->kind==ConstValue::Kind::Float))return v; if(u->op=="-"){if(v->kind==ConstValue::Kind::Int){if(v->intValue==std::numeric_limits<int64_t>::min()){constError(expr->line,expr->col,"integer overflow in constant expression");return std::nullopt;}v->intValue=-v->intValue;return v;}if(v->kind==ConstValue::Kind::Float){v->floatValue=-v->floatValue;return v;}} return std::nullopt; }
    if(auto* b=dynamic_cast<const BinaryExpr*>(expr)) return evalBinaryConst(*b);
    if(auto* t=dynamic_cast<const TernaryExpr*>(expr)){auto c=evaluateConstExpr(t->cond.get());if(!c)return std::nullopt;bool x;if(!evalAsBool(*c,x))return std::nullopt;return evaluateConstExpr(x?t->thenExpr.get():t->elseExpr.get());}
    if(auto* n=dynamic_cast<const NullCoalesceExpr*>(expr)){auto l=evaluateConstExpr(n->left.get());if(!l)return std::nullopt;return l->kind==ConstValue::Kind::Null?evaluateConstExpr(n->fallback.get()):l;}
    if(auto* e=dynamic_cast<const ElvisExpr*>(expr)){auto l=evaluateConstExpr(e->left.get());if(!l)return std::nullopt;return l->kind==ConstValue::Kind::Null?evaluateConstExpr(e->fallback.get()):l;}
    if(auto* c=dynamic_cast<const AsCastExpr*>(expr)){auto v=evaluateConstExpr(c->target.get());if(!v)return std::nullopt;auto target=normalizeTypeName(c->typeName);if(target=="Float"&&v->kind==ConstValue::Kind::Int){ConstValue o;o.kind=ConstValue::Kind::Float;o.floatValue=(double)v->intValue;return o;}if(target=="Int"&&v->kind==ConstValue::Kind::Float&&std::isfinite(v->floatValue)&&std::trunc(v->floatValue)==v->floatValue&&v->floatValue>=static_cast<double>(std::numeric_limits<int64_t>::min())&&v->floatValue<=static_cast<double>(std::numeric_limits<int64_t>::max())){ConstValue o;o.kind=ConstValue::Kind::Int;o.intValue=(int64_t)v->floatValue;return o;}if((target=="Int"&&v->kind==ConstValue::Kind::Int)||(target=="Float"&&v->kind==ConstValue::Kind::Float)||(target=="Bool"&&v->kind==ConstValue::Kind::Bool)||(target=="String"&&v->kind==ConstValue::Kind::String))return v;return std::nullopt;}
    return std::nullopt;
}

bool TypeChecker::evalAsBool(const ConstValue& v,bool& out)const{if(v.kind!=ConstValue::Kind::Bool)return false;out=v.boolValue;return true;}
bool TypeChecker::evalAsNumber(const ConstValue& v,long double& out,bool& isFloat)const{if(v.kind==ConstValue::Kind::Int){out=(long double)v.intValue;isFloat=false;return true;}if(v.kind==ConstValue::Kind::Float){out=(long double)v.floatValue;isFloat=true;return true;}return false;}

std::optional<ConstValue> TypeChecker::evalBinaryConst(const BinaryExpr& e){
    if(e.op=="&&"||e.op=="||"){auto l=evaluateConstExpr(e.left.get());if(!l)return std::nullopt;bool a;if(!evalAsBool(*l,a))return std::nullopt;if(e.op=="&&"&&!a){ConstValue o;o.kind=ConstValue::Kind::Bool;o.boolValue=false;return o;}if(e.op=="||"&&a){ConstValue o;o.kind=ConstValue::Kind::Bool;o.boolValue=true;return o;}auto r=evaluateConstExpr(e.right.get());if(!r)return std::nullopt;bool b;if(!evalAsBool(*r,b))return std::nullopt;ConstValue o;o.kind=ConstValue::Kind::Bool;o.boolValue=e.op=="&&"?(a&&b):(a||b);return o;}
    auto l=evaluateConstExpr(e.left.get()), r=evaluateConstExpr(e.right.get()); if(!l||!r)return std::nullopt;
    if(e.op=="=="||e.op=="!="){bool eq=false;if(l->kind==r->kind)switch(l->kind){case ConstValue::Kind::Int:eq=l->intValue==r->intValue;break;case ConstValue::Kind::Float:eq=l->floatValue==r->floatValue;break;case ConstValue::Kind::Bool:eq=l->boolValue==r->boolValue;break;case ConstValue::Kind::String:eq=l->stringValue==r->stringValue;break;case ConstValue::Kind::Null:eq=true;break;}ConstValue o;o.kind=ConstValue::Kind::Bool;o.boolValue=e.op=="=="?eq:!eq;return o;}
    if(e.op=="+"&&l->kind==ConstValue::Kind::String&&r->kind==ConstValue::Kind::String){ConstValue o;o.kind=ConstValue::Kind::String;o.stringValue=l->stringValue+r->stringValue;return o;}
    long double a,b;bool af,bf;if(!evalAsNumber(*l,a,af)||!evalAsNumber(*r,b,bf))return std::nullopt;
    if(e.op=="%") {if(af||bf||r->intValue==0){if(r->intValue==0)constError(e.line,e.col,"division by zero in constant expression");return std::nullopt;}if(l->intValue==std::numeric_limits<int64_t>::min()&&r->intValue==-1){constError(e.line,e.col,"integer overflow in constant expression");return std::nullopt;}ConstValue o;o.kind=ConstValue::Kind::Int;o.intValue=l->intValue%r->intValue;return o;}
    if(e.op=="/"&&b==0){constError(e.line,e.col,"division by zero in constant expression");return std::nullopt;}
    if(e.op=="<"||e.op=="<="||e.op==">"||e.op==">="){ConstValue o;o.kind=ConstValue::Kind::Bool;if(e.op=="<")o.boolValue=a<b;else if(e.op=="<=")o.boolValue=a<=b;else if(e.op==">")o.boolValue=a>b;else o.boolValue=a>=b;return o;}
    long double result;if(e.op=="+")result=a+b;else if(e.op=="-")result=a-b;else if(e.op=="*")result=a*b;else if(e.op=="/")result=a/b;else if(e.op=="^")result=std::pow(a,b);else return std::nullopt;
    const bool floating=af||bf||e.op=="/"||e.op=="^"; if(!std::isfinite((double)result)){constError(e.line,e.col,"non-finite result in constant expression");return std::nullopt;}
    if(!floating&&std::floor(result)==result&&result>=(long double)std::numeric_limits<int64_t>::min()&&result<=(long double)std::numeric_limits<int64_t>::max()){ConstValue o;o.kind=ConstValue::Kind::Int;o.intValue=(int64_t)result;return o;}
    ConstValue o;o.kind=ConstValue::Kind::Float;o.floatValue=(double)result;return o;
}

std::string TypeChecker::constValueText(const ConstValue& value) const { return value.toString(); }
void TypeChecker::publishConst(const DeclarationStmt& decl,const ConstValue& value){constScopes.back()[decl.name]=value;foldedConsts[decl.name]=value;semantic.symbols().setConstValue(decl.name,decl.line,decl.col,constValueText(value));}

} // namespace nova
