#include "IRBuilder.hpp"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

namespace nova::ir {

// ============================================================================
// IR.hpp implementation
// ============================================================================

static const char* opcodeName(Opcode op) {
    switch (op) {
        case Opcode::Nop: return "nop"; case Opcode::Alloc: return "alloc";
        case Opcode::Load: return "load"; case Opcode::Store: return "store";
        case Opcode::Add: return "add"; case Opcode::Sub: return "sub";
        case Opcode::Mul: return "mul"; case Opcode::Div: return "div";
        case Opcode::Mod: return "mod"; case Opcode::Pow: return "pow";
        case Opcode::Neg: return "neg"; case Opcode::Not: return "not";
        case Opcode::Eq: return "eq"; case Opcode::Ne: return "ne";
        case Opcode::Lt: return "lt"; case Opcode::Le: return "le";
        case Opcode::Gt: return "gt"; case Opcode::Ge: return "ge";
        case Opcode::And: return "and"; case Opcode::Or: return "or";
        case Opcode::Call: return "call"; case Opcode::Jump: return "jump";
        case Opcode::CondBranch: return "condbr"; case Opcode::Return: return "ret";
        case Opcode::Phi: return "phi"; case Opcode::Select: return "select";
        case Opcode::Cast: return "cast"; case Opcode::Index: return "index";
        case Opcode::Member: return "member"; case Opcode::MakeArray: return "make_array";
        case Opcode::MakeMap: return "make_map"; case Opcode::MakeTuple: return "make_tuple";
        case Opcode::Slice: return "slice"; case Opcode::Invoke: return "invoke";
        case Opcode::Await: return "await"; case Opcode::Yield: return "yield";
        case Opcode::AsyncSuspend: return "async_suspend";
        case Opcode::AsyncResume: return "async_resume";
        case Opcode::ErrorCheck: return "error_check";
        case Opcode::Runtime: return "runtime";
    }
    return "unknown";
}

std::string Type::str() const {
    if (!name.empty() && (kind == TypeKind::Pointer || kind == TypeKind::Aggregate ||
                          kind == TypeKind::Function)) {
        if (parameters.empty()) return name;
        std::ostringstream os; os << name << "<";
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            if (i) os << ",";
            os << parameters[i].str();
        }
        os << ">"; return os.str();
    }
    switch (kind) {
        case TypeKind::Void: return "Void"; case TypeKind::Bool: return "Bool";
        case TypeKind::Int: return "Int"; case TypeKind::Float: return "Float";
        case TypeKind::String: return "String"; case TypeKind::Null: return "Null";
        case TypeKind::Any: return "Any"; default: return name.empty() ? "Any" : name;
    }
}
bool Type::operator==(const Type& o) const {
    if (kind != o.kind || name != o.name || parameters.size() != o.parameters.size()) return false;
    for (std::size_t i=0;i<parameters.size();++i) if (parameters[i] != o.parameters[i]) return false;
    return true;
}
std::string Value::ref() const {
    if (kind == ValueKind::Constant) return name.empty() ? "<const>" : name;
    if (id) return "%" + std::to_string(id);
    return name.empty() ? "%undef" : "%" + name;
}
bool Instruction::isTerminator() const {
    return opcode == Opcode::Jump || opcode == Opcode::CondBranch ||
           opcode == Opcode::Return || opcode == Opcode::Invoke;
}
InstructionPtr BasicBlock::terminator() const {
    if (instructions.empty() || !instructions.back()->isTerminator()) return {};
    return instructions.back();
}
BasicBlock* Function::entry() const {
    return blocks.empty() ? nullptr : blocks.front().get();
}
BasicBlock* Function::createBlock(const std::string& blockName) {
    auto id = static_cast<std::uint64_t>(blocks.size());
    blocks.push_back(std::make_unique<BasicBlock>(id, blockName));
    return blocks.back().get();
}
Function* Module::createFunction(const std::string& functionName, Type ret) {
    functions.push_back(std::make_unique<Function>());
    auto* f = functions.back().get(); f->name = functionName; f->returnType = std::move(ret);
    return f;
}
Function* Module::findFunction(const std::string& functionName) const {
    for (const auto& f : functions) if (f->name == functionName) return f.get();
    return nullptr;
}

std::string print(const Module& m) {
    std::ostringstream os;
    os << "module " << m.name << "\n";
    for (const auto& i : m.imports) os << "import " << i << "\n";
    for (const auto& f : m.functions) {
        os << "\nfunction " << f->name << "(";
        for (std::size_t i=0;i<f->arguments.size();++i) {
            if (i) os << ", ";
            os << f->arguments[i]->ref() << ": " << f->arguments[i]->type.str();
        }
        os << ") -> " << f->returnType.str() << "\n";
        for (const auto& b : f->blocks) {
            os << b->name << ":\n";
            for (const auto& ins : b->instructions) {
                os << "  ";
                if (ins->type.kind != TypeKind::Void && ins->opcode != Opcode::Store &&
                    ins->opcode != Opcode::Jump && ins->opcode != Opcode::CondBranch &&
                    ins->opcode != Opcode::Return && ins->opcode != Opcode::Invoke)
                    os << ins->ref() << " = ";
                os << opcodeName(ins->opcode);
                for (const auto& v : ins->operands) os << " " << (v ? v->ref() : "<null>");
                if (!ins->successors.empty()) {
                    os << " ->";
                    for (auto* s : ins->successors) os << " " << s->name;
                }
                os << "\n";
            }
        }
    }
    return os.str();
}

// ============================================================================
// IRBuilder
// ============================================================================

IRBuilder::IRBuilder() = default;

std::unique_ptr<Module> IRBuilder::build(const nova::Program& program) {
    module_ = std::make_unique<Module>();
    genericDecls_.clear();
    nextValueId_ = 1;
    module_->name = "nova";
    lowerTopLevel(program);
    return std::move(module_);
}

std::string IRBuilder::uniqueName(const std::string& p) {
    return p + "." + std::to_string(ctx_ ? ctx_->tempCounter++ : 0);
}

BasicBlock* IRBuilder::makeBlock(const std::string& hint) {
    if (!ctx_ || !ctx_->function) throw std::logic_error("IRBuilder: no active function");
    return ctx_->function->createBlock(hint + "." + std::to_string(ctx_->function->blocks.size()));
}
void IRBuilder::setCurrent(BasicBlock* bb) { ctx_->current = bb; }

void IRBuilder::branch(BasicBlock* target) {
    if (!ctx_->current || ctx_->current->terminated()) return;
    auto i = std::make_shared<Instruction>(Type::voidTy(), Opcode::Jump);
    i->successors = {target};
    ctx_->current->instructions.push_back(i);
    ctx_->current->successors.push_back(target);
    target->predecessors.push_back(ctx_->current);
}
void IRBuilder::condBranch(const ValuePtr& c, BasicBlock* yes, BasicBlock* no) {
    if (!ctx_->current || ctx_->current->terminated()) return;
    auto i = std::make_shared<Instruction>(Type::voidTy(), Opcode::CondBranch);
    i->operands = {c}; i->successors = {yes,no};
    ctx_->current->instructions.push_back(i);
    ctx_->current->successors.push_back(yes); ctx_->current->successors.push_back(no);
    yes->predecessors.push_back(ctx_->current); no->predecessors.push_back(ctx_->current);
}
void IRBuilder::ensureOpen(BasicBlock* continuationHint) {
    if (ctx_->current && ctx_->current->terminated()) {
        setCurrent(continuationHint ? continuationHint : makeBlock("unreachable.cont"));
    }
}

ValuePtr IRBuilder::emit(Opcode op, Type type, std::vector<ValuePtr> operands, const std::string& hint) {
    if (!ctx_ || !ctx_->current) throw std::logic_error("IRBuilder: no insertion point");
    auto i = std::make_shared<Instruction>(std::move(type), op, hint.empty() ? uniqueName("t") : hint);
    i->operands = std::move(operands);
    i->id = nextValueId_++;
    ctx_->current->instructions.push_back(i);
    return i;
}
ValuePtr IRBuilder::emitVoid(Opcode op, std::vector<ValuePtr> operands, const std::string& hint) {
    return emit(op, Type::voidTy(), std::move(operands), hint);
}
ValuePtr IRBuilder::makeConstant(const Type& type, const std::string& text) {
    auto c = std::make_shared<ConstantValue>(type, text);
    c->name = text; return c;
}
ValuePtr IRBuilder::makeUndef(const Type& type) {
    return std::make_shared<Value>(type, "undef", ValueKind::Undef);
}
ValuePtr IRBuilder::makePhi(const Type& type, const std::vector<PhiIncoming>& in, const std::string& hint) {
    auto v = emit(Opcode::Phi, type, {}, hint.empty() ? uniqueName("phi") : hint);
    auto p = std::dynamic_pointer_cast<Instruction>(v);
    for (const auto& x : in) {
        if (x.value) p->operands.push_back(x.value);
        if (x.predecessor) p->attributes["pred" + std::to_string(p->operands.size()-1)] =
            x.predecessor->name;
    }
    return v;
}
void IRBuilder::addPhiIncoming(const ValuePtr& phi, const ValuePtr& value, BasicBlock* pred) {
    auto p = std::dynamic_pointer_cast<Instruction>(phi);
    if (!p) throw std::logic_error("attempt to patch non-phi");
    p->operands.push_back(value);
    p->attributes["pred" + std::to_string(p->operands.size()-1)] = pred ? pred->name : "";
}

// ---------- Type helpers ----------
Type IRBuilder::typeFromName(const std::string& n) const {
    if (n == "Void") return Type::voidTy();
    if (n == "Bool" || n == "boolean") return Type::boolTy();
    if (n == "Int" || n == "Integer" || n == "i64") return Type::intTy();
    if (n == "Float" || n == "Double" || n == "f64") return Type::floatTy();
    if (n == "String" || n == "str") return Type::stringTy();
    if (n == "Null" || n == "null") return Type::nullTy();
    if (n == "Any" || n.empty()) return Type::anyTy();
    Type t; t.kind = TypeKind::Aggregate; t.name = n; return t;
}
Type IRBuilder::typeFromRef(const TypeRef& r) const {
    Type t = typeFromName(r.name);
    if (!r.generics.empty()) {
        t.kind = TypeKind::Aggregate; t.name = r.name;
        for (const auto& g : r.generics) t.parameters.push_back(typeFromRef(g));
    }
    return t;
}
Type IRBuilder::inferType(const Expr& e) const {
    if (auto* x=dynamic_cast<const LiteralExpr*>(&e)) return typeFromName(
        x->kind==LiteralKind::Int?"Int":x->kind==LiteralKind::Float?"Float":
        x->kind==LiteralKind::String||x->kind==LiteralKind::RawString?"String":
        x->kind==LiteralKind::Bool?"Bool":x->kind==LiteralKind::Null?"Null":"Any");
    if (auto* x=dynamic_cast<const BinaryExpr*>(&e)) {
        if (x->op=="=="||x->op=="!="||x->op=="<"||x->op==">"||x->op=="<="||x->op==">="||
            x->op=="&&"||x->op=="||") return Type::boolTy();
        return inferType(*x->left);
    }
    if (auto* x=dynamic_cast<const UnaryExpr*>(&e)) return x->op=="!" ? Type::boolTy() : inferType(*x->operand);
    if (auto* x=dynamic_cast<const TernaryExpr*>(&e)) return inferType(*x->thenExpr);
    if (auto* x=dynamic_cast<const ElvisExpr*>(&e)) return inferType(*x->left);
    if (auto* x=dynamic_cast<const NullCoalesceExpr*>(&e)) return inferType(*x->fallback);
    if (auto* x=dynamic_cast<const ArrayLiteralExpr*>(&e)) {
        Type t; t.kind=TypeKind::Aggregate; t.name="Array"; if(!x->elements.empty()) t.parameters.push_back(inferType(*x->elements.front())); return t;
    }
    if (auto* x=dynamic_cast<const TupleLiteralExpr*>(&e)) {
        Type t; t.kind=TypeKind::Aggregate; t.name="Tuple"; for(auto& e2:x->elements) t.parameters.push_back(inferType(*e2)); return t;
    }
    if (auto* x=dynamic_cast<const IdentifierExpr*>(&e)) {
        if (ctx_) { auto it=ctx_->values.find(x->name); if(it!=ctx_->values.end()) return it->second->type; }
    }
    return Type::anyTy();
}

std::string IRBuilder::mangleGenericName(const std::string& base, const std::vector<Type>& types) const {
    std::ostringstream os; os << base << "$";
    for (std::size_t i=0;i<types.size();++i) { if(i) os << "_"; os << types[i].str(); }
    return os.str();
}
std::string IRBuilder::mangleGeneric(const FunctionDecl& fn, const std::vector<Type>& args) const {
    return mangleGenericName(fn.name, args);
}

// ---------- Module/function ----------
void IRBuilder::lowerTopLevel(const nova::Program& p) {
    // Top-level executable statements live in a synthetic entry function.
    auto* top = module_->createFunction("__nova_main", Type::voidTy());
    FunctionContext local;
    ctx_ = &local;
    ctx_->function = top; ctx_->current = top->createBlock("entry");
    for (const auto& s : p.statements) lowerStmt(*s);
    emitReturnIfNeeded();
    ctx_ = nullptr;
}

void IRBuilder::lowerFunction(const FunctionDecl& fn, const std::string& forcedName) {
    std::string name = forcedName.empty() ? fn.name : forcedName;
    auto* f = module_->findFunction(name);
    if (!f) f = module_->createFunction(name, Type::anyTy());
    f->isAsync = fn.isAsync; f->isGeneric = !fn.generics.empty(); f->genericParameters = fn.generics;
    if (f->isGeneric) genericDecls_[fn.name] = &fn;
    if (fn.isAsync) f->attributes["lowering"] = "state-machine";

    FunctionContext local;
    FunctionContext* previous = ctx_;
    ctx_ = &local; local.function=f; local.current=f->createBlock("entry");

    for (const auto& p : fn.params) {
        auto arg = std::make_shared<Value>(Type::anyTy(), p.name, ValueKind::Argument);
        arg->id = nextValueId_++;
        f->arguments.push_back(arg); local.values[p.name]=arg;
        if (p.defaultValue) {
            // Default expressions are emitted only as a fallback runtime hook.
            auto def = lowerExpr(*p.defaultValue);
            local.values[p.name] = def;
        }
    }
    if (fn.body) lowerBlock(*fn.body);
    emitReturnIfNeeded();
    ctx_ = previous;
}
void IRBuilder::emitReturnIfNeeded() {
    if (!ctx_ || !ctx_->current || ctx_->current->terminated()) return;
    emitVoid(Opcode::Return, {});
}

void IRBuilder::lowerClass(const ClassDecl& c) {
    // A class itself is a runtime type declaration; methods become ordinary
    // functions with a stable Class.Method symbol.
    for (const auto& m : c.members) if (m.isMethod && m.method) {
        lowerFunction(*m.method, c.name + "." + m.method->name);
    }
}
void IRBuilder::lowerModuleDecl(const ModuleDecl& m) {
    if (!m.body) return;
    // Namespace/module declarations are represented in symbol names.
    for (const auto& s : m.body->statements) {
        if (auto* f=dynamic_cast<const FunctionDecl*>(s.get())) lowerFunction(*f, m.name+"."+f->name);
        else lowerStmt(*s);
    }
}

// ---------- Statements ----------
void IRBuilder::lowerBlock(const Block& b) {
    for (const auto& s : b.statements) {
        if (!ctx_->current || ctx_->current->terminated()) break;
        lowerStmt(*s);
    }
}
void IRBuilder::lowerStmt(const Stmt& s) {
    if (auto* x=dynamic_cast<const DeclarationStmt*>(&s)) return lowerDeclaration(*x);
    if (auto* x=dynamic_cast<const AssignmentStmt*>(&s)) return lowerAssignment(*x);
    if (auto* x=dynamic_cast<const DestructuringStmt*>(&s)) return lowerDestructuring(*x);
    if (auto* x=dynamic_cast<const ExprStmt*>(&s)) { lowerExpr(*x->expr); return; }
    if (auto* x=dynamic_cast<const IfStmt*>(&s)) return lowerIf(*x);
    if (auto* x=dynamic_cast<const SwitchStmt*>(&s)) return lowerSwitch(*x);
    if (auto* x=dynamic_cast<const ForStmt*>(&s)) return lowerFor(*x);
    if (auto* x=dynamic_cast<const WhileStmt*>(&s)) return lowerWhile(*x);
    if (auto* x=dynamic_cast<const RepeatStmt*>(&s)) return lowerRepeat(*x);
    if (auto* x=dynamic_cast<const ForeachStmt*>(&s)) return lowerForeach(*x);
    if (dynamic_cast<const BreakStmt*>(&s)) { if(ctx_->loops.empty()) throw std::runtime_error("break outside loop"); branch(ctx_->loops.back().breakTarget); return; }
    if (dynamic_cast<const ContinueStmt*>(&s)) { if(ctx_->loops.empty()) throw std::runtime_error("continue outside loop"); branch(ctx_->loops.back().continueTarget); return; }
    if (auto* x=dynamic_cast<const ReturnStmt*>(&s)) return lowerReturn(*x);
    if (auto* x=dynamic_cast<const YieldStmt*>(&s)) { auto v=lowerExpr(*x->value); emitVoid(Opcode::Yield,{v}); return; }
    if (auto* x=dynamic_cast<const FunctionDecl*>(&s)) return lowerFunction(*x);
    if (auto* x=dynamic_cast<const ClassDecl*>(&s)) return lowerClass(*x);
    if (auto* x=dynamic_cast<const ModuleDecl*>(&s)) return lowerModuleDecl(*x);
    if (auto* x=dynamic_cast<const ImportStmt*>(&s)) { module_->imports.push_back(x->path); return; }
    if (auto* x=dynamic_cast<const ExportStmt*>(&s)) { module_->exports.push_back(x->name); return; }
    if (auto* x=dynamic_cast<const TryStmt*>(&s)) return lowerTry(*x);
    if (auto* x=dynamic_cast<const ThreadStmt*>(&s)) return lowerThread(*x);
    if (auto* x=dynamic_cast<const EventStmt*>(&s)) return lowerEvent(*x);
    if (auto* x=dynamic_cast<const UnsafeStmt*>(&s)) return lowerUnsafe(*x);
    if (auto* x=dynamic_cast<const SignalDecl*>(&s)) return lowerSignal(*x);
    if (auto* x=dynamic_cast<const UsingStmt*>(&s)) return lowerUsing(*x);
    if (auto* x=dynamic_cast<const GuardStmt*>(&s)) return lowerGuard(*x);
    if (dynamic_cast<const TypeAliasStmt*>(&s)) return;
    if (auto* x=dynamic_cast<const ExtendStmt*>(&s)) return lowerExtend(*x);
    if (auto* x=dynamic_cast<const LazyDecl*>(&s)) return lowerLazy(*x);
    if (auto* x=dynamic_cast<const ComptimeStmt*>(&s)) return lowerComptime(*x);
    if (auto* x=dynamic_cast<const ChanDecl*>(&s)) return lowerChan(*x);
    if (auto* x=dynamic_cast<const AnnotatedStmt*>(&s)) return lowerAnnotated(*x);
    // Interfaces/enums/structs/macros are type/symbol declarations. They have
    // no executable SSA instructions by themselves.
}

void IRBuilder::lowerDeclaration(const DeclarationStmt& s) {
    ValuePtr v = s.value ? lowerExpr(*s.value) : makeUndef(s.typeAnnotation ? typeFromRef(*s.typeAnnotation) : Type::anyTy());
    if (s.typeAnnotation) {
        Type declared=typeFromRef(*s.typeAnnotation);
        if (v->type != declared) v=emit(Opcode::Cast, declared,{v},"cast."+s.name);
    }
    ctx_->values[s.name]=v;
}
void IRBuilder::lowerAssignment(const AssignmentStmt& s) {
    auto* id=dynamic_cast<const IdentifierExpr*>(s.target.get());
    ValuePtr rhs=lowerExpr(*s.value);
    if (id) {
        ValuePtr lhs;
        auto it=ctx_->values.find(id->name);
        if(it!=ctx_->values.end()) lhs=it->second;
        if(s.op!="=" && lhs) {
            Opcode op=Opcode::Add;
            if(s.op=="+=") op=Opcode::Add; else if(s.op=="-=") op=Opcode::Sub;
            else if(s.op=="*=") op=Opcode::Mul; else if(s.op=="/=") op=Opcode::Div;
            rhs=emit(op,lhs->type,{lhs,rhs},id->name);
        }
        ctx_->values[id->name]=rhs; return;
    }
    // Addressable aggregates use explicit store operations. This makes the
    // subsequent SSA/memory-to-register pass straightforward.
    if (auto* m=dynamic_cast<const MemberExpr*>(s.target.get())) {
        auto base=lowerExpr(*m->target);
        auto addr=emit(Opcode::Member,Type{TypeKind::Pointer,m->name,{}},{base},"addr."+m->name);
        emitVoid(Opcode::Store,{addr,rhs}); return;
    }
    if (auto* ix=dynamic_cast<const IndexExpr*>(s.target.get())) {
        auto base=lowerExpr(*ix->target), idx=lowerExpr(*ix->index);
        auto addr=emit(Opcode::Index,Type{TypeKind::Pointer,"element",{}},{base,idx});
        emitVoid(Opcode::Store,{addr,rhs}); return;
    }
    throw std::runtime_error("Nova IR: unsupported assignment target");
}
void IRBuilder::lowerDestructuring(const DestructuringStmt& s) {
    auto v=lowerExpr(*s.value);
    for(std::size_t i=0;i<s.targets.size();++i) {
        auto idx=makeConstant(Type::intTy(),std::to_string(i));
        ctx_->values[s.targets[i]]=emit(Opcode::Index,Type::anyTy(),{v,idx},"destructure."+s.targets[i]);
    }
}

void IRBuilder::lowerIf(const IfStmt& s) {
    auto before=*ctx_;
    auto* thenBB=makeBlock("if.then");
    auto* elseBB=makeBlock("if.else");
    auto* joinBB=makeBlock("if.join");
    auto c=lowerExpr(*s.condition);
    condBranch(c,thenBB,elseBB);

    setCurrent(thenBB); lowerBlock(*s.thenBranch);
    auto thenEnv=*ctx_; BasicBlock* thenEnd=ctx_->current;
    if(thenEnd && !thenEnd->terminated()) branch(joinBB);

    // Restore the pre-if SSA environment before generating the other arm.
    ctx_->values=before.values; ctx_->slots=before.slots; ctx_->loops=before.loops;
    setCurrent(elseBB);
    if(s.elseBranch) lowerBlock(*s.elseBranch);
    else if(s.elseIf) lowerIf(*s.elseIf);
    auto elseEnv=*ctx_; BasicBlock* elseEnd=ctx_->current;
    if(elseEnd && !elseEnd->terminated()) branch(joinBB);

    setCurrent(joinBB);
    // A branch that returns/breaks/etc. does not contribute an incoming edge.
    const auto reachable=[&](BasicBlock* b){ return b && std::find(joinBB->predecessors.begin(),joinBB->predecessors.end(),b)!=joinBB->predecessors.end(); };
    std::unordered_map<std::string,ValuePtr> merged=before.values;
    std::unordered_map<std::string,ValuePtr> keys=before.values;
    for(auto& kv:thenEnv.values) keys.emplace(kv.first,kv.second);
    for(auto& kv:elseEnv.values) keys.emplace(kv.first,kv.second);
    for(auto& kv:keys) {
        ValuePtr a=before.values.count(kv.first)?before.values.at(kv.first):makeUndef(Type::anyTy());
        ValuePtr tv=thenEnv.values.count(kv.first)?thenEnv.values.at(kv.first):a;
        ValuePtr ev=elseEnv.values.count(kv.first)?elseEnv.values.at(kv.first):a;
        if(tv==ev) { merged[kv.first]=tv; continue; }
        std::vector<PhiIncoming> in;
        if(reachable(thenEnd)) in.push_back({tv,thenEnd});
        if(reachable(elseEnd)) in.push_back({ev,elseEnd});
        merged[kv.first]=in.size()==1 ? in.front().value : makePhi(tv->type,in,"phi."+kv.first);
    }
    ctx_->values=std::move(merged);
    ctx_->slots=before.slots; ctx_->loops=before.loops;
}

void IRBuilder::lowerSwitch(const SwitchStmt& s) {
    auto subject=lowerExpr(*s.subject);
    auto* join=makeBlock("switch.join");
    std::vector<BasicBlock*> caseBB;
    for(std::size_t i=0;i<s.cases.size();++i) caseBB.push_back(makeBlock("switch.case"));
    auto* def=makeBlock("switch.default");

    // Build a linear decision tree: compare subject against each case value.
    BasicBlock* decision=ctx_->current;
    for(std::size_t i=0;i<s.cases.size();++i) {
        setCurrent(decision);
        auto cv=lowerExpr(*s.cases[i].value);
        auto eq=emit(Opcode::Eq,Type::boolTy(),{subject,cv});
        BasicBlock* next=(i+1<caseBB.size())?makeBlock("switch.next"):def;
        condBranch(eq,caseBB[i],next);
        decision=next;
    }
    if(s.cases.empty()) { setCurrent(decision); branch(def); }
    setCurrent(def);
    for(const auto& st:s.defaultBody) lowerStmt(*st);
    if(!ctx_->current->terminated()) branch(join);
    for(std::size_t i=0;i<s.cases.size();++i) {
        setCurrent(caseBB[i]);
        for(const auto& st:s.cases[i].body) { if(!ctx_->current->terminated()) lowerStmt(*st); }
        if(!ctx_->current->terminated()) branch(join);
    }
    setCurrent(join);
}

void IRBuilder::lowerWhile(const WhileStmt& s) {
    auto pre=ctx_->current; auto* header=makeBlock("while.header");
    auto* body=makeBlock("while.body"); auto* exit=makeBlock("while.exit");
    branch(header); setCurrent(header);

    // Loop-carried SSA variables become phi nodes at the header.
    auto initial=ctx_->values; std::unordered_map<std::string,ValuePtr> phis;
    for(auto& kv:initial) {
        auto phi=makePhi(kv.second->type,{{kv.second,pre}},"phi.loop."+kv.first);
        phis[kv.first]=phi; ctx_->values[kv.first]=phi;
    }
    auto cond=lowerExpr(*s.condition); condBranch(cond,body,exit);
    setCurrent(body);
    ctx_->loops.push_back({header,exit});
    lowerBlock(*s.body); ctx_->loops.pop_back();
    auto bodyEnv=*ctx_; BasicBlock* bodyEnd=ctx_->current;
    if(bodyEnd && !bodyEnd->terminated()) branch(header);
    if(bodyEnd && bodyEnd->terminated()) {
        // A terminated body cannot provide a back-edge unless it explicitly jumped.
        if(std::find(header->predecessors.begin(),header->predecessors.end(),bodyEnd)==header->predecessors.end())
            bodyEnd=nullptr;
    }
    if(bodyEnd) for(auto& kv:phis) {
        ValuePtr v=bodyEnv.values.count(kv.first)?bodyEnv.values.at(kv.first):kv.second;
        addPhiIncoming(kv.second,v,bodyEnd);
    }
    setCurrent(exit);
}
void IRBuilder::lowerFor(const ForStmt& s) {
    // Canonical counted loop:
    // preheader -> header(phi) -> body -> latch -> header, with exit from header.
    auto start=lowerExpr(*s.from), limit=lowerExpr(*s.to);
    auto step=s.step?lowerExpr(*s.step):makeConstant(Type::intTy(),"1");
    ctx_->values[s.varName]=start;
    auto pre=ctx_->current; auto* header=makeBlock("for.header");
    auto* body=makeBlock("for.body"); auto* latch=makeBlock("for.latch"); auto* exit=makeBlock("for.exit");
    branch(header); setCurrent(header);
    auto phi=makePhi(start->type,{{start,pre}},"phi."+s.varName);
    ctx_->values[s.varName]=phi;
    auto cond=emit(Opcode::Lt,Type::boolTy(),{phi,limit});
    condBranch(cond,body,exit);
    setCurrent(body); ctx_->loops.push_back({latch,exit}); if(s.body) lowerBlock(*s.body); ctx_->loops.pop_back();
    auto bodyEnv=*ctx_; BasicBlock* bodyEnd=ctx_->current;
    if(bodyEnd && !bodyEnd->terminated()) branch(latch);
    setCurrent(latch);
    auto cur=bodyEnv.values.count(s.varName)?bodyEnv.values.at(s.varName):phi;
    auto next=emit(Opcode::Add,start->type,{cur,step},"for.next");
    branch(header); addPhiIncoming(phi,next,latch);
    setCurrent(exit);
}
void IRBuilder::lowerRepeat(const RepeatStmt& s) {
    // Lower repeat N as a counted loop with an internal induction variable.
    auto n=lowerExpr(*s.count), zero=makeConstant(Type::intTy(),"0");
    std::string name=uniqueName("__repeat_i");
    ctx_->values[name]=zero;
    auto fake=ForStmt{}; (void)fake;
    auto pre=ctx_->current; auto* header=makeBlock("repeat.header");
    auto* body=makeBlock("repeat.body"); auto* exit=makeBlock("repeat.exit");
    branch(header); setCurrent(header);
    auto phi=makePhi(Type::intTy(),{{zero,pre}},"repeat.phi");
    ctx_->values[name]=phi;
    auto cond=emit(Opcode::Lt,Type::boolTy(),{phi,n}); condBranch(cond,body,exit);
    setCurrent(body); ctx_->loops.push_back({nullptr,exit}); ctx_->loops.back().continueTarget=header;
    lowerBlock(*s.body); ctx_->loops.pop_back();
    auto* end=ctx_->current;
    if(end && !end->terminated()) {
        setCurrent(end); auto next=emit(Opcode::Add,Type::intTy(),{phi,makeConstant(Type::intTy(),"1")});
        branch(header); addPhiIncoming(phi,next,end);
    }
    setCurrent(exit); ctx_->values.erase(name);
}
void IRBuilder::lowerForeach(const ForeachStmt& s) {
    auto iterable=lowerExpr(*s.iterable);
    auto begin=runtimeCall("nova.iter.begin",{iterable},Type::anyTy());
    auto pre=ctx_->current; auto* header=makeBlock("foreach.header");
    auto* body=makeBlock("foreach.body"); auto* exit=makeBlock("foreach.exit");
    branch(header); setCurrent(header);
    auto itphi=makePhi(Type::anyTy(),{{begin,pre}},"foreach.iter");
    auto has=runtimeCall("nova.iter.has",{itphi},Type::boolTy());
    condBranch(has,body,exit);
    setCurrent(body);
    ctx_->values[s.varName]=runtimeCall("nova.iter.value",{itphi});
    ctx_->loops.push_back({header,exit}); if(s.body) lowerBlock(*s.body); ctx_->loops.pop_back();
    auto* end=ctx_->current;
    if(end && !end->terminated()) {
        auto next=runtimeCall("nova.iter.next",{itphi},Type::anyTy());
        branch(header); addPhiIncoming(itphi,next,end);
    }
    setCurrent(exit);
}
void IRBuilder::lowerReturn(const ReturnStmt& s) {
    if(s.value) {
        auto v=lowerExpr(*s.value);
        auto i=std::make_shared<Instruction>(Type::voidTy(),Opcode::Return);
        i->operands={v}; ctx_->current->instructions.push_back(i);
    } else emitVoid(Opcode::Return,{});
}
void IRBuilder::lowerTry(const TryStmt& s) {
    auto* catchBB=makeBlock("catch");
    auto* normalBB=makeBlock("try.normal");
    auto* finallyBB=makeBlock("finally");
    auto oldTarget=ctx_->errorTarget; auto oldError=ctx_->errorValue;
    ctx_->errorTarget=catchBB;
    runtimeCall("nova.try.begin",{},Type::voidTy());
    if(s.tryBlock) lowerBlock(*s.tryBlock);
    auto* tryEnd=ctx_->current;
    if(tryEnd && !tryEnd->terminated()) branch(normalBB);
    setCurrent(catchBB);
    ctx_->errorValue=runtimeCall("nova.error.current",{},Type::anyTy());
    if(!s.catchVar.empty()) ctx_->values[s.catchVar]=ctx_->errorValue;
    if(s.catchBlock) lowerBlock(*s.catchBlock);
    auto* catchEnd=ctx_->current;
    if(catchEnd && !catchEnd->terminated()) branch(finallyBB);
    setCurrent(normalBB); runtimeCall("nova.try.end",{},Type::voidTy());
    if(!ctx_->current->terminated()) branch(finallyBB);
    setCurrent(finallyBB);
    if(s.finallyBlock) lowerBlock(*s.finallyBlock);
    ctx_->errorTarget=oldTarget; ctx_->errorValue=oldError;
}
void IRBuilder::lowerThread(const ThreadStmt& s) {
    // Thread bodies are lowered into a normal function and launched by runtime.
    // We cannot clone unique_ptr AST nodes. Emit a runtime closure hook instead;
    // the body itself is still lowered in the current function.
    auto fnName=module_->name+".thread."+s.name;
    runtimeCall("nova.thread.spawn",{makeConstant(Type::stringTy(),fnName)},Type::voidTy());
    if(s.body) lowerBlock(*s.body);
}
void IRBuilder::lowerEvent(const EventStmt& s) {
    runtimeCall("nova.event.on",{makeConstant(Type::stringTy(),s.eventName)},Type::voidTy());
    if(s.body) lowerBlock(*s.body);
}
void IRBuilder::lowerUnsafe(const UnsafeStmt& s) {
    emitVoid(Opcode::Runtime,{makeConstant(Type::stringTy(),"nova.unsafe.begin")});
    if(s.body) lowerBlock(*s.body);
    if(ctx_->current && !ctx_->current->terminated()) emitVoid(Opcode::Runtime,{makeConstant(Type::stringTy(),"nova.unsafe.end")});
}
void IRBuilder::lowerSignal(const SignalDecl& s) {
    auto v=s.initial?lowerExpr(*s.initial):makeUndef(Type::anyTy());
    ctx_->values[s.name]=runtimeCall("nova.signal.create",{makeConstant(Type::stringTy(),s.name),v});
}
void IRBuilder::lowerUsing(const UsingStmt& s) {
    auto r=lowerExpr(*s.resource); ctx_->values[s.varName]=r;
    runtimeCall("nova.resource.enter",{r},Type::voidTy());
    if(s.body) lowerBlock(*s.body);
    if(ctx_->current && !ctx_->current->terminated()) runtimeCall("nova.resource.exit",{r},Type::voidTy());
}
void IRBuilder::lowerGuard(const GuardStmt& s) {
    auto cond=lowerExpr(*s.condition); auto* ok=makeBlock("guard.ok"); auto* fail=makeBlock("guard.fail");
    condBranch(cond,ok,fail); setCurrent(fail); if(s.elseBlock) lowerBlock(*s.elseBlock);
    if(!ctx_->current->terminated()) branch(ok);
    setCurrent(ok);
}
void IRBuilder::lowerLazy(const LazyDecl& s) {
    auto name=makeConstant(Type::stringTy(),s.name);
    auto v=s.initializer?lowerExpr(*s.initializer):makeUndef(Type::anyTy());
    ctx_->values[s.name]=runtimeCall("nova.lazy.create",{name,v});
}
void IRBuilder::lowerComptime(const ComptimeStmt& s) {
    // The semantic/comptime phase normally consumes this before IR generation.
    // If it survives, preserve it as an explicitly marked runtime-free region.
    emitVoid(Opcode::Runtime,{makeConstant(Type::stringTy(),"nova.comptime.begin")});
    if(s.body) lowerBlock(*s.body);
}
void IRBuilder::lowerExtend(const ExtendStmt& s) {
    for(const auto& m:s.methods) if(m) lowerFunction(*m,s.typeName+"."+m->name);
}
void IRBuilder::lowerChan(const ChanDecl& s) {
    ctx_->values[s.name]=runtimeCall("nova.channel.create",{makeConstant(Type::stringTy(),s.elementType.name)});
}
void IRBuilder::lowerAnnotated(const AnnotatedStmt& a) {
    if(auto* f=dynamic_cast<const FunctionDecl*>(a.inner.get())) {
        // Preserve annotation names on the resulting function.
        lowerFunction(*f);
        if(auto* irf=module_->findFunction(f->name)) for(const auto& an:a.annotations) irf->attributes["annotation."+an.name]="true";
        return;
    }
    if(a.inner) lowerStmt(*a.inner);
}

// ---------- Expressions ----------
ValuePtr IRBuilder::lowerExpr(const Expr& e) {
    if(auto*x=dynamic_cast<const LiteralExpr*>(&e)) return lowerLiteral(*x);
    if(auto*x=dynamic_cast<const IdentifierExpr*>(&e)) return lowerIdentifier(*x);
    if(auto*x=dynamic_cast<const BinaryExpr*>(&e)) return lowerBinary(*x);
    if(auto*x=dynamic_cast<const UnaryExpr*>(&e)) return lowerUnary(*x);
    if(auto*x=dynamic_cast<const CallExpr*>(&e)) return lowerCall(*x);
    if(auto*x=dynamic_cast<const MemberExpr*>(&e)) return lowerMember(*x);
    if(auto*x=dynamic_cast<const IndexExpr*>(&e)) return lowerIndex(*x);
    if(auto*x=dynamic_cast<const SliceExpr*>(&e)) return lowerSlice(*x);
    if(auto*x=dynamic_cast<const ArrayLiteralExpr*>(&e)) return lowerArray(*x);
    if(auto*x=dynamic_cast<const MapLiteralExpr*>(&e)) return lowerMap(*x);
    if(auto*x=dynamic_cast<const TupleLiteralExpr*>(&e)) return lowerTuple(*x);
    if(auto*x=dynamic_cast<const LambdaExpr*>(&e)) return lowerLambda(*x);
    if(auto*x=dynamic_cast<const TernaryExpr*>(&e)) return lowerTernary(*x);
    if(auto*x=dynamic_cast<const ElvisExpr*>(&e)) return lowerElvis(*x);
    if(auto*x=dynamic_cast<const NullCoalesceExpr*>(&e)) return lowerNullCoalesce(*x);
    if(auto*x=dynamic_cast<const RangeExpr*>(&e)) return lowerRange(*x);
    if(auto*x=dynamic_cast<const MatchExpr*>(&e)) return lowerMatchExpr(*x);
    if(auto*x=dynamic_cast<const AsCastExpr*>(&e)) return lowerCast(*x);
    if(auto*x=dynamic_cast<const ExistsExpr*>(&e)) return lowerExists(*x);
    if(auto*x=dynamic_cast<const SpreadExpr*>(&e)) return lowerSpread(*x);
    if(auto*x=dynamic_cast<const DictComprehensionExpr*>(&e)) return lowerDictComprehension(*x);
    throw std::runtime_error("Nova IR: unsupported AST expression");
}
ValuePtr IRBuilder::lowerLiteral(const LiteralExpr& e) {
    switch(e.kind) {
        case LiteralKind::Int: return makeConstant(Type::intTy(),std::to_string(e.intValue));
        case LiteralKind::Float: { std::ostringstream os; os<<std::setprecision(17)<<e.floatValue; return makeConstant(Type::floatTy(),os.str()); }
        case LiteralKind::String: case LiteralKind::RawString: return makeConstant(Type::stringTy(),e.stringValue);
        case LiteralKind::Bool: return makeConstant(Type::boolTy(),e.boolValue?"true":"false");
        case LiteralKind::Null: return makeConstant(Type::nullTy(),"null");
    }
    return makeUndef(Type::anyTy());
}
ValuePtr IRBuilder::lowerIdentifier(const IdentifierExpr& e) {
    auto it=ctx_->values.find(e.name);
    if(it!=ctx_->values.end()) return it->second;
    // Unresolved names are normally rejected by semantic analysis. Keeping an
    // explicit runtime symbol makes diagnostics/IR dumping possible.
    return runtimeCall("nova.symbol.load",{makeConstant(Type::stringTy(),e.name)});
}
ValuePtr IRBuilder::lowerBinary(const BinaryExpr& e) {
    if(e.op=="&&" || e.op=="||") {
        // Short-circuiting is real CFG, not a bitwise approximation.
        auto lhs=lowerExpr(*e.left); auto* rhsBB=makeBlock("logic.rhs");
        auto* shortBB=makeBlock("logic.short"); auto* join=makeBlock("logic.join");
        if(e.op=="&&") condBranch(lhs,rhsBB,shortBB); else condBranch(lhs,shortBB,rhsBB);
        setCurrent(shortBB); auto shortV=makeConstant(Type::boolTy(),e.op=="&&"?"false":"true"); branch(join);
        auto* shortEnd=ctx_->current;
        setCurrent(rhsBB); auto rhs=lowerExpr(*e.right); branch(join); auto* rhsEnd=ctx_->current;
        setCurrent(join);
        return makePhi(Type::boolTy(),{{shortV,shortEnd},{rhs,rhsEnd}},"logic.phi");
    }
    auto l=lowerExpr(*e.left), r=lowerExpr(*e.right);
    static const std::unordered_map<std::string,Opcode> ops={
        {"+",Opcode::Add},{"-",Opcode::Sub},{"*",Opcode::Mul},{"/",Opcode::Div},
        {"%",Opcode::Mod},{"^",Opcode::Pow},{"==",Opcode::Eq},{"!=",Opcode::Ne},
        {"<",Opcode::Lt},{"<=",Opcode::Le},{">",Opcode::Gt},{">=",Opcode::Ge}
    };
    if(e.op=="|>") return runtimeCall("nova.pipeline",{l,r});
    auto it=ops.find(e.op); if(it==ops.end()) return runtimeCall("nova.binary."+e.op,{l,r});
    Type t=(e.op=="=="||e.op=="!="||e.op=="<"||e.op=="<="||e.op==">"||e.op==">=")?Type::boolTy():l->type;
    return emit(it->second,t,{l,r});
}
ValuePtr IRBuilder::lowerUnary(const UnaryExpr& e) {
    if(e.op=="await") {
        if(auto* c=dynamic_cast<const CallExpr*>(e.operand.get())) return lowerAwaitCall(*c);
        auto v=lowerExpr(*e.operand); emitVoid(Opcode::AsyncSuspend,{v}); return emit(Opcode::Await,Type::anyTy(),{v});
    }
    auto v=lowerExpr(*e.operand);
    if(e.op=="!") return emit(Opcode::Not,Type::boolTy(),{v});
    if(e.op=="-") return emit(Opcode::Neg,v->type,{v});
    if(e.op=="++" || e.op=="--") {
        auto one=makeConstant(Type::intTy(),"1");
        auto n=emit(e.op=="++"?Opcode::Add:Opcode::Sub,v->type,{v,one});
        if(auto* id=dynamic_cast<const IdentifierExpr*>(e.operand.get())) ctx_->values[id->name]=n;
        return n;
    }
    return runtimeCall("nova.unary."+e.op,{v});
}
ValuePtr IRBuilder::lowerCall(const CallExpr& e) {
    if(e.errorPropagate) {
        std::vector<ValuePtr> args; std::vector<Type> types;
        for(const auto& a:e.args) { auto v=lowerExpr(*a); args.push_back(v); types.push_back(v->type); }
        std::string callee;
        if(auto* id=dynamic_cast<const IdentifierExpr*>(e.callee.get())) {
            callee=id->name;
            if(auto* f=module_->findFunction(callee); f && f->isGeneric)
                callee=mangleGenericName(callee,types);
        } else if(auto* m=dynamic_cast<const MemberExpr*>(e.callee.get())) {
            auto target=lowerExpr(*m->target); args.insert(args.begin(),target); callee=m->name;
        } else {
            auto fn=lowerExpr(*e.callee); args.insert(args.begin(),fn); callee="__indirect";
        }
        args.insert(args.begin(),makeConstant(Type::stringTy(),callee));
        auto raw=runtimeCall("nova.call",args,Type::anyTy());
        return lowerErrorPropagation(raw,"call");
    }
    if(ctx_->function->isAsync) {
        // Async lowering is triggered by an explicit UnaryExpr("await"), not
        // by every call. Ordinary calls remain ordinary calls.
    }
    std::vector<ValuePtr> args; std::vector<Type> types;
    for(const auto& a:e.args) { auto v=lowerExpr(*a); args.push_back(v); types.push_back(v->type); }
    std::string callee;
    if(auto* id=dynamic_cast<const IdentifierExpr*>(e.callee.get())) {
        callee=id->name;
        if(auto* f=module_->findFunction(callee); f && f->isGeneric) callee=mangleGenericName(callee,types);
    } else if(auto* m=dynamic_cast<const MemberExpr*>(e.callee.get())) {
        auto target=lowerExpr(*m->target); args.insert(args.begin(),target);
        callee=m->name;
    } else {
        auto fn=lowerExpr(*e.callee); args.insert(args.begin(),fn); callee="__indirect";
    }
    args.insert(args.begin(),makeConstant(Type::stringTy(),callee));
    return runtimeCall("nova.call",args,Type::anyTy());
}
ValuePtr IRBuilder::lowerMember(const MemberExpr& e) {
    auto base=lowerExpr(*e.target);
    if(e.safeNav) {
        auto nonnull=runtimeCall("nova.is_nonnull",{base},Type::boolTy());
        auto* yes=makeBlock("safe.yes"), *no=makeBlock("safe.no"), *join=makeBlock("safe.join");
        condBranch(nonnull,yes,no);
        setCurrent(no); auto nv=makeConstant(Type::nullTy(),"null"); branch(join); auto* ne=ctx_->current;
        setCurrent(yes); auto mv=emit(Opcode::Member,Type::anyTy(),{base},e.name); branch(join); auto* me=ctx_->current;
        setCurrent(join); return makePhi(Type::anyTy(),{{nv,ne},{mv,me}},"safe.phi");
    }
    return emit(Opcode::Member,Type::anyTy(),{base},e.name);
}
ValuePtr IRBuilder::lowerIndex(const IndexExpr& e) {
    return emit(Opcode::Index,Type::anyTy(),{lowerExpr(*e.target),lowerExpr(*e.index)});
}
ValuePtr IRBuilder::lowerSlice(const SliceExpr& e) {
    std::vector<ValuePtr> a={lowerExpr(*e.target)};
    a.push_back(e.start?lowerExpr(*e.start):makeConstant(Type::nullTy(),"null"));
    a.push_back(e.end?lowerExpr(*e.end):makeConstant(Type::nullTy(),"null"));
    a.push_back(e.step?lowerExpr(*e.step):makeConstant(Type::nullTy(),"null"));
    return emit(Opcode::Slice,Type::anyTy(),a);
}
ValuePtr IRBuilder::lowerArray(const ArrayLiteralExpr& e) {
    std::vector<ValuePtr> a; for(auto& x:e.elements) a.push_back(lowerExpr(*x));
    return emit(Opcode::MakeArray,Type{TypeKind::Aggregate,"Array",{}},a);
}
ValuePtr IRBuilder::lowerMap(const MapLiteralExpr& e) {
    std::vector<ValuePtr> a; for(auto& x:e.entries){a.push_back(lowerExpr(*x.key));a.push_back(lowerExpr(*x.value));}
    return emit(Opcode::MakeMap,Type{TypeKind::Aggregate,"Map",{}},a);
}
ValuePtr IRBuilder::lowerTuple(const TupleLiteralExpr& e) {
    std::vector<ValuePtr> a; for(auto& x:e.elements)a.push_back(lowerExpr(*x));
    return emit(Opcode::MakeTuple,Type{TypeKind::Aggregate,"Tuple",{}},a);
}
ValuePtr IRBuilder::lowerLambda(const LambdaExpr& e) {
    // Lambda AST owns its body and cannot be cloned. Lower it into a runtime
    // closure descriptor; closure conversion can capture the current SSA
    // values without requiring an AST clone.
    std::vector<ValuePtr> caps;
    for(auto& kv:ctx_->values) caps.push_back(kv.second);
    std::vector<ValuePtr> args={makeConstant(Type::stringTy(),"lambda")};
    args.insert(args.end(),caps.begin(),caps.end());
    return runtimeCall("nova.closure.create",args,Type{TypeKind::Function,"lambda",{}});
}
ValuePtr IRBuilder::lowerTernary(const TernaryExpr& e) {
    auto c=lowerExpr(*e.cond); auto* y=makeBlock("ternary.yes"),*n=makeBlock("ternary.no"),*j=makeBlock("ternary.join");
    condBranch(c,y,n); setCurrent(y); auto a=lowerExpr(*e.thenExpr); branch(j); auto* ae=ctx_->current;
    setCurrent(n); auto b=lowerExpr(*e.elseExpr); branch(j); auto* be=ctx_->current;
    setCurrent(j); return makePhi(a->type,{{a,ae},{b,be}},"ternary.phi");
}
ValuePtr IRBuilder::lowerElvis(const ElvisExpr& e) {
    auto l=lowerExpr(*e.left), ok=runtimeCall("nova.is_truthy",{l},Type::boolTy());
    auto* yes=makeBlock("elvis.yes"),*no=makeBlock("elvis.no"),*j=makeBlock("elvis.join");
    condBranch(ok,yes,no); setCurrent(yes); branch(j); auto* ye=ctx_->current;
    setCurrent(no); auto f=lowerExpr(*e.fallback); branch(j); auto* ne=ctx_->current;
    setCurrent(j); return makePhi(l->type,{{l,ye},{f,ne}},"elvis.phi");
}
ValuePtr IRBuilder::lowerNullCoalesce(const NullCoalesceExpr& e) {
    auto l=lowerExpr(*e.left), ok=runtimeCall("nova.is_nonnull",{l},Type::boolTy());
    auto* yes=makeBlock("coalesce.yes"),*no=makeBlock("coalesce.no"),*j=makeBlock("coalesce.join");
    condBranch(ok,yes,no); setCurrent(yes); branch(j); auto* ye=ctx_->current;
    setCurrent(no); auto f=lowerExpr(*e.fallback); branch(j); auto* ne=ctx_->current;
    setCurrent(j); return makePhi(l->type,{{l,ye},{f,ne}},"coalesce.phi");
}
ValuePtr IRBuilder::lowerRange(const RangeExpr& e) {
    std::vector<ValuePtr> a={lowerExpr(*e.from),lowerExpr(*e.to)};
    a.push_back(e.step?lowerExpr(*e.step):makeConstant(Type::intTy(),"1"));
    return runtimeCall("nova.range",a,Type{TypeKind::Aggregate,"Range",{}});
}
ValuePtr IRBuilder::lowerMatchExpr(const MatchExpr& e) {
    // Match expressions use the same decision-CFG pattern as switch and return
    // one SSA phi at the common join.
    auto subject=lowerExpr(*e.subject); auto* join=makeBlock("match.join");
    std::vector<std::pair<BasicBlock*,ValuePtr>> arms;
    BasicBlock* decision=ctx_->current;
    for(std::size_t i=0;i<e.cases.size();++i) {
        auto* arm=makeBlock("match.case"); auto* next=makeBlock("match.next");
        setCurrent(decision); auto p=lowerExpr(*e.cases[i].pattern);
        auto eq=emit(Opcode::Eq,Type::boolTy(),{subject,p}); condBranch(eq,arm,next);
        setCurrent(arm);
        ValuePtr val=runtimeCall("nova.match.body",{});
        if(e.cases[i].body) lowerStmt(*e.cases[i].body);
        if(!ctx_->current->terminated()) branch(join);
        arms.push_back({ctx_->current,val}); decision=next;
    }
    setCurrent(decision); ValuePtr def=makeUndef(Type::anyTy());
    if(e.defaultBody) { lowerStmt(*e.defaultBody); if(!ctx_->current->terminated()) branch(join); }
    else branch(join);
    auto* de=decision; setCurrent(join);
    std::vector<PhiIncoming> in; for(auto& a:arms) in.push_back({a.second,a.first}); in.push_back({def,de});
    return makePhi(Type::anyTy(),in,"match.phi");
}
ValuePtr IRBuilder::lowerCast(const AsCastExpr& e) {
    auto v=lowerExpr(*e.target); Type t=typeFromName(e.typeName);
    if(e.safe) return runtimeCall("nova.safe_cast",{v,makeConstant(Type::stringTy(),e.typeName)},t);
    return emit(Opcode::Cast,t,{v});
}
ValuePtr IRBuilder::lowerExists(const ExistsExpr& e) {
    return runtimeCall("nova.exists",{lowerExpr(*e.target)},Type::boolTy());
}
ValuePtr IRBuilder::lowerSpread(const SpreadExpr& e) {
    return runtimeCall("nova.spread",{lowerExpr(*e.target)});
}
ValuePtr IRBuilder::lowerDictComprehension(const DictComprehensionExpr& e) {
    std::vector<ValuePtr> a={lowerExpr(*e.iterable),makeConstant(Type::stringTy(),"dict_comprehension")};
    if(e.condition) a.push_back(lowerExpr(*e.condition));
    a.push_back(lowerExpr(*e.keyExpr)); a.push_back(lowerExpr(*e.valueExpr));
    return runtimeCall("nova.dict.comprehension",a,Type{TypeKind::Aggregate,"Map",{}});
}

// ---------- runtime / async / error ----------
ValuePtr IRBuilder::runtimeCall(const std::string& name,const std::vector<ValuePtr>& args,Type result) {
    std::vector<ValuePtr> all; all.push_back(makeConstant(Type::stringTy(),name)); all.insert(all.end(),args.begin(),args.end());
    return emit(Opcode::Runtime,result,std::move(all),name);
}
ValuePtr IRBuilder::lowerAwaitCall(const CallExpr& call) {
    auto result=lowerCall(call);
    emitVoid(Opcode::AsyncSuspend,{result});
    auto awaited=emit(Opcode::Await,Type::anyTy(),{result},"await");
    emitVoid(Opcode::AsyncResume,{awaited});
    return awaited;
}
ValuePtr IRBuilder::lowerErrorPropagation(const ValuePtr& result,const std::string& sourceName) {
    auto check=emit(Opcode::ErrorCheck,Type::boolTy(),{result},sourceName+".ok");
    auto* ok=makeBlock("error.ok"), *err=makeBlock("error.propagate");
    condBranch(check,ok,err);
    setCurrent(err);
    if(ctx_->errorTarget) {
        ctx_->errorValue=runtimeCall("nova.error.extract",{result});
        branch(ctx_->errorTarget);
    } else {
        auto ex=runtimeCall("nova.error.extract",{result});
        auto i=std::make_shared<Instruction>(Type::voidTy(),Opcode::Return);
        i->operands={ex}; ctx_->current->instructions.push_back(i);
    }
    setCurrent(ok);
    return runtimeCall("nova.error.unwrap",{result});
}

} // namespace nova::ir
