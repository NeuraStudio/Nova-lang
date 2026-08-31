#include <optional>
// LLVMBackend.cpp — implementation. See LLVMBackend.hpp for the ABI
// contract this file compiles against; read that header's top comment
// before making any change here.

#include "LLVMBackend.hpp"

#include <llvm/IR/Verifier.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/PassManager.h>

#include <system_error>

namespace nova::backend {

using llvm::Type;
using LLVMValue = llvm::Value;

// ═══════════════════════════════ lifecycle ═══════════════════════════════

LLVMBackend::LLVMBackend(BackendOptions options) : options_(std::move(options)) {
    llvmContext_ = std::make_unique<llvm::LLVMContext>();
    llvmModule_ = std::make_unique<llvm::Module>("nova_module", *llvmContext_);
    builder_ = std::make_unique<llvm::IRBuilder<>>(*llvmContext_);
}

LLVMBackend::~LLVMBackend() = default;

void LLVMBackend::diag(const std::string& message) {
    diagnostics_.push_back(message);
}

std::pair<std::unique_ptr<llvm::LLVMContext>, std::unique_ptr<llvm::Module>> LLVMBackend::takeModule() {
    return {std::move(llvmContext_), std::move(llvmModule_)};
}

// ═══════════════════════════════ type lowering ═══════════════════════════════
//
// nova::ir::Type -> llvm::Type. Per the header's contract: concretely-typed
// scalars (Int/Float/Bool/Void) get real native LLVM types so the numeric
// fast path (lowerNumericBinary) can operate on them directly. Every other
// TypeKind (String/Null/Any/Pointer/Aggregate/Function, and Int/Float/Bool
// wherever they show up boxed as an operand of a Runtime instruction) is
// represented as `%NovaValue*` — an opaque boxed value — because that is
// the only representation IR guarantees is available uniformly (see the
// header comment: IRBuilder overwhelmingly produces TypeKind::Any).

llvm::Type* LLVMBackend::lowerType(const nova::ir::Type& t) {
    switch (t.kind) {
        case nova::ir::TypeKind::Void:
            return llvm::Type::getVoidTy(*llvmContext_);
        case nova::ir::TypeKind::Bool:
            return llvm::Type::getInt1Ty(*llvmContext_);
        case nova::ir::TypeKind::Int:
            return llvm::Type::getInt64Ty(*llvmContext_);
        case nova::ir::TypeKind::Float:
            return llvm::Type::getDoubleTy(*llvmContext_);
        case nova::ir::TypeKind::Pointer:
            // A pointer-to-pointer over the element type when known, else a
            // generic NovaValue* — nova::ir's Pointer kind is used sparingly
            // by IRBuilder today, so this keeps both cases correct.
            if (!t.parameters.empty()) {
                llvm::Type* pointee = lowerType(t.parameters[0]);
                return llvm::PointerType::getUnqual(pointee);
            }
            return novaValuePtrTy_;
        case nova::ir::TypeKind::String:
        case nova::ir::TypeKind::Null:
        case nova::ir::TypeKind::Any:
        case nova::ir::TypeKind::Aggregate:
        case nova::ir::TypeKind::Function:
        default:
            return novaValuePtrTy_;
    }
}

bool LLVMBackend::isNumericType(const nova::ir::Type& t) const {
    return t.kind == nova::ir::TypeKind::Int ||
           t.kind == nova::ir::TypeKind::Float ||
           t.kind == nova::ir::TypeKind::Bool;
}

// ═══════════════════════════════ runtime ABI declarations ═══════════════════════════════

void LLVMBackend::declareOpaqueNovaValueType() {
    // %NovaValue = type opaque
    novaValueOpaqueTy_ = llvm::StructType::create(*llvmContext_, "NovaValue");
    novaValuePtrTy_ = llvm::PointerType::getUnqual(novaValueOpaqueTy_);
}

llvm::FunctionCallee LLVMBackend::declareRuntimeFn(const std::string& name,
                                                    llvm::Type* returnTy,
                                                    std::vector<llvm::Type*> paramTys,
                                                    bool isVarArg) {
    auto it = runtimeFns_.find(name);
    if (it != runtimeFns_.end()) return it->second;

    llvm::FunctionType* fnTy = llvm::FunctionType::get(returnTy, paramTys, isVarArg);
    llvm::FunctionCallee callee = llvmModule_->getOrInsertFunction(name, fnTy);
    runtimeFns_[name] = callee;

    // Mark as a plain C ABI, no-throw runtime helper (Nova's own error
    // model is represented explicitly via nova_rt_error_check + boxed
    // NovaValue results, not C++ exceptions crossing this boundary).
    if (auto* fn = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
        fn->setDoesNotThrow();
    }
    return callee;
}

// Declares every entry point listed in the LLVMBackend.hpp ABI contract.
// Called once, eagerly, right after the opaque NovaValue type exists — so
// every lowering path below can assume the function it wants is already
// declared and just look it up via runtimeFns_[name] (through
// declareRuntimeFn's memoized getOrInsertFunction, which is idempotent).
void LLVMBackend::declareAllRuntimeFns() {
    auto& ctx = *llvmContext_;
    llvm::Type* i8p   = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx));
    llvm::Type* i64   = llvm::Type::getInt64Ty(ctx);
    llvm::Type* i32   = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i1    = llvm::Type::getInt1Ty(ctx);
    llvm::Type* dbl   = llvm::Type::getDoubleTy(ctx);
    llvm::Type* voidT = llvm::Type::getVoidTy(ctx);
    llvm::Type* nvp   = novaValuePtrTy_;
    llvm::Type* nvpp  = llvm::PointerType::getUnqual(nvp); // NovaValue**

    declareRuntimeFn("nova_rt_call",          nvp,   {i8p, nvpp, i64});
    declareRuntimeFn("nova_rt_alloc",         nvp,   {i64});
    declareRuntimeFn("nova_rt_load",          nvp,   {nvp});
    declareRuntimeFn("nova_rt_store",         voidT, {nvp, nvp});

    declareRuntimeFn("nova_rt_const_int",     nvp,   {i64});
    declareRuntimeFn("nova_rt_const_float",   nvp,   {dbl});
    declareRuntimeFn("nova_rt_const_string",  nvp,   {i8p});
    declareRuntimeFn("nova_rt_const_bool",    nvp,   {i1});
    declareRuntimeFn("nova_rt_const_null",    nvp,   {});

    declareRuntimeFn("nova_rt_to_int",        i64,   {nvp});
    declareRuntimeFn("nova_rt_to_float",      dbl,   {nvp});
    declareRuntimeFn("nova_rt_to_bool",       i1,    {nvp});
    declareRuntimeFn("nova_rt_from_int",      nvp,   {i64});
    declareRuntimeFn("nova_rt_from_float",    nvp,   {dbl});
    declareRuntimeFn("nova_rt_from_bool",     nvp,   {i1});

    for (const char* op : {"add", "sub", "mul", "div", "mod", "pow",
                            "eq", "ne", "lt", "le", "gt", "ge", "and", "or"}) {
        declareRuntimeFn(std::string("nova_rt_") + op, nvp, {nvp, nvp});
    }
    declareRuntimeFn("nova_rt_neg",           nvp, {nvp});
    declareRuntimeFn("nova_rt_not",           nvp, {nvp});
    declareRuntimeFn("nova_rt_select",        nvp, {nvp, nvp, nvp});
    declareRuntimeFn("nova_rt_cast",          nvp, {nvp, i8p});
    declareRuntimeFn("nova_rt_index",         nvp, {nvp, nvp});
    declareRuntimeFn("nova_rt_member",        nvp, {nvp, i8p});
    declareRuntimeFn("nova_rt_slice",         nvp, {nvp, nvp, nvp, nvp});
    declareRuntimeFn("nova_rt_make_array",    nvp, {nvpp, i64});
    declareRuntimeFn("nova_rt_make_map",      nvp, {nvpp, i64});
    declareRuntimeFn("nova_rt_make_tuple",    nvp, {nvpp, i64});

    declareRuntimeFn("nova_rt_error_check",   i1,  {nvp});
    declareRuntimeFn("nova_rt_await",         nvp, {nvp});
    declareRuntimeFn("nova_rt_yield",         nvp, {nvp});
    declareRuntimeFn("nova_rt_async_suspend", voidT, {nvp});
    declareRuntimeFn("nova_rt_async_resume",  voidT, {nvp});

    (void)i32; // reserved for future exit-code plumbing (nova_main's return type)
}

// ═══════════════════════════════ module lowering entry point ═══════════════════════════════

bool LLVMBackend::lower(const nova::ir::Module& novaModule) {
    diagnostics_.clear();
    llvmModule_->setModuleIdentifier(novaModule.name.empty() ? "nova_module" : novaModule.name);

    declareOpaqueNovaValueType();
    declareAllRuntimeFns();

    // Pass 1: declare every function's LLVM signature up front, so any
    // function's body can reference any other function's llvm::Function*
    // regardless of declaration order (Nova, like most languages, allows
    // forward references within a module).
    for (const auto& fnPtr : novaModule.functions) {
        llvm::Function* llvmFn = declareNovaFunction(*fnPtr);
        functionMap_[fnPtr->name] = llvmFn;
    }

    // Pass 2: lower each function's actual body (blocks + instructions).
    bool ok = true;
    for (const auto& fnPtr : novaModule.functions) {
        llvm::Function* llvmFn = functionMap_.at(fnPtr->name);
        lowerFunctionBody(*fnPtr, llvmFn);
    }

    // Global values declared at module scope (nova::ir::Module::globals).
    // Represented as internal NovaValue* global variables, lazily
    // initialized the first time they're referenced isn't modeled by
    // nova::ir today (no init-order info is carried), so we conservatively
    // emit them as a global NovaValue* initialized to null; a future
    // "module init function" pass can populate these from `globals`'
    // ValuePtr if/when IRBuilder starts recording an initializer expression
    // rather than just a name -> ValuePtr placeholder.
    for (const auto& kv : novaModule.globals) {
        const std::string& gname = kv.first;
        if (llvmModule_->getGlobalVariable(gname)) continue;
        new llvm::GlobalVariable(
            *llvmModule_, novaValuePtrTy_, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantPointerNull::get(novaValuePtrTy_),
            gname);
    }

    // Verify the module structurally (catches any lowering bug — mismatched
    // block terminators, dangling phi predecessors, etc. — before it ever
    // reaches the target-independent optimizer or codegen).
    std::string verifyErr;
    llvm::raw_string_ostream os(verifyErr);
    if (llvm::verifyModule(*llvmModule_, &os)) {
        diag("module verification failed:\n" + os.str());
        ok = false;
    }

    return ok;
}

// Declares (but does not fill in) one nova::ir::Function as an llvm::Function.
// Per the header contract, every Nova function parameter and return value is
// a boxed NovaValue* at the LLVM level (nova::ir::Function::returnType is
// Type::anyTy() for essentially every function IRBuilder produces, and
// arguments are always Type::anyTy() too — see IRBuilder::lowerFunction).
// A concretely-typed return (e.g. a function IRBuilder someday annotates
// with a real Int/Float/Bool returnType) still lowers correctly here since
// lowerType() handles both cases.
llvm::Function* LLVMBackend::declareNovaFunction(const nova::ir::Function& fn) {
    std::vector<llvm::Type*> paramTys;
    paramTys.reserve(fn.arguments.size());
    for (const auto& arg : fn.arguments) {
        paramTys.push_back(lowerType(arg->type));
    }
    llvm::Type* retTy = lowerType(fn.returnType);
    llvm::FunctionType* fnTy = llvm::FunctionType::get(retTy, paramTys, /*isVarArg=*/false);

    // Nova function names may collide with the runtime ABI's own "nova_rt_"
    // prefix only by user error; guard with a "nova_fn_" prefix so a Nova
    // source function literally named e.g. "call" cannot alias nova_rt_call.
    std::string linkName = "nova_fn_" + fn.name;
    llvm::Function* llvmFn = llvm::Function::Create(
        fnTy, llvm::Function::ExternalLinkage, linkName, llvmModule_.get());

    std::size_t i = 0;
    for (auto& param : llvmFn->args()) {
        if (i < fn.arguments.size()) param.setName(fn.arguments[i]->name);
        ++i;
    }
    return llvmFn;
}

// ═══════════════════════════════ function body lowering (two-pass SSA -> LLVM) ═══════════════════════════════

void LLVMBackend::lowerFunctionBody(const nova::ir::Function& novaFn, llvm::Function* llvmFn) {
    valueMap_.clear();
    blockMap_.clear();

    if (novaFn.blocks.empty()) {
        // A declared-but-empty function (e.g. an interface stub) — give it
        // a single block that returns a sane default so the module still
        // verifies; real interface methods never reach codegen in practice
        // since InterfaceDecl bodies are never lowered by IRBuilder.
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(*llvmContext_, "entry", llvmFn);
        builder_->SetInsertPoint(bb);
        if (llvmFn->getReturnType()->isVoidTy()) builder_->CreateRetVoid();
        else builder_->CreateRet(llvm::Constant::getNullValue(llvmFn->getReturnType()));
        return;
    }

    // Pass A: create every llvm::BasicBlock up front (named after the
    // nova::ir block, deduped by LLVM automatically via numeric suffixes),
    // and bind each nova::ir::Value argument to its llvm::Argument.
    for (const auto& bbPtr : novaFn.blocks) {
        blockMap_[bbPtr.get()] = llvm::BasicBlock::Create(*llvmContext_, bbPtr->name, llvmFn);
    }
    {
        std::size_t i = 0;
        for (auto& llvmArg : llvmFn->args()) {
            if (i < novaFn.arguments.size()) valueMap_[novaFn.arguments[i].get()] = &llvmArg;
            ++i;
        }
    }

    // Pass B: lower every instruction in every block, in nova::ir order.
    // Phi nodes are created eagerly (so any later block can resolve them as
    // an operand) but their incoming (value, predecessor) edges are patched
    // in Pass C, once every instruction in the function has a llvm::Value*
    // — this mirrors exactly what nova::ir itself does (IRBuilder::makePhi
    // creates the Phi instruction before all its incoming values may exist,
    // then patches edges via addPhiIncoming/attributes["predN"]).
    std::vector<PendingPhi> pendingPhis;
    for (const auto& bbPtr : novaFn.blocks) {
        llvm::BasicBlock* llvmBB = blockMap_.at(bbPtr.get());
        builder_->SetInsertPoint(llvmBB);
        for (const auto& instPtr : bbPtr->instructions) {
            llvm::Value* result = lowerInstruction(*instPtr, pendingPhis);
            if (result) valueMap_[instPtr.get()] = result;
        }
        // Guard against a nova::ir block that (due to an upstream bug) has
        // no terminator — every LLVM block must end in exactly one. This
        // should never trigger against a correctly-formed IR module, but a
        // silent LLVM verifier failure is much harder to debug than an
        // explicit diagnostic here.
        if (!llvmBB->getTerminator()) {
            diag("block '" + bbPtr->name + "' in function '" + novaFn.name +
                 "' has no terminating instruction (unreachable-fallthrough patched with 'ret')");
            if (llvmFn->getReturnType()->isVoidTy()) builder_->CreateRetVoid();
            else builder_->CreateRet(llvm::Constant::getNullValue(llvmFn->getReturnType()));
        }
    }

    // Pass C: patch every deferred phi's incoming edges now that every
    // instruction in the function has a resolved llvm::Value*.
    for (const auto& pending : pendingPhis) {
        const nova::ir::Instruction& novaPhi = *pending.novaPhi;
        for (std::size_t idx = 0; idx < novaPhi.operands.size(); ++idx) {
            const nova::ir::ValuePtr& incomingValue = novaPhi.operands[idx];
            llvm::Value* llvmIncoming = resolveOperand(incomingValue);

            // nova::ir::IRBuilder::makePhi records the predecessor's NAME
            // (not pointer) in attributes["pred" + index] (see IR.hpp /
            // IRBuilder.cpp: makePhi -> attributes["pred"+idx] = predecessor->name).
            // Resolve that name back to this function's llvm::BasicBlock.
            llvm::BasicBlock* predBB = nullptr;
            auto attrIt = novaPhi.attributes.find("pred" + std::to_string(idx));
            if (attrIt != novaPhi.attributes.end()) {
                for (const auto& bbPtr : novaFn.blocks) {
                    if (bbPtr->name == attrIt->second) { predBB = blockMap_.at(bbPtr.get()); break; }
                }
            }
            if (!predBB) {
                diag("phi '" + novaPhi.name + "' in function '" + novaFn.name +
                     "': could not resolve predecessor block for incoming operand " +
                     std::to_string(idx) + " (skipped)");
                continue;
            }
            if (!llvmIncoming) {
                diag("phi '" + novaPhi.name + "' in function '" + novaFn.name +
                     "': incoming value " + std::to_string(idx) + " did not resolve (using undef)");
                llvmIncoming = llvm::UndefValue::get(pending.llvmPhi->getType());
            }
            // LLVM phis require the incoming value's type to exactly match
            // the phi's declared type; box/unbox as needed rather than
            // trusting nova::ir's per-branch type inference to always agree
            // (e.g. one arm concretely Int, the other still Any after a
            // runtime call — a completely legal and common shape).
            if (llvmIncoming->getType() != pending.llvmPhi->getType()) {
                llvm::IRBuilder<> edgeBuilder(predBB->getTerminator());
                if (pending.llvmPhi->getType() == novaValuePtrTy_ && isNumericType(novaPhi.type) == false) {
                    // Target is boxed, incoming is a raw scalar -> box it.
                    if (llvmIncoming->getType()->isIntegerTy(64))
                        llvmIncoming = edgeBuilder.CreateCall(runtimeFns_.at("nova_rt_from_int"), {llvmIncoming});
                    else if (llvmIncoming->getType()->isDoubleTy())
                        llvmIncoming = edgeBuilder.CreateCall(runtimeFns_.at("nova_rt_from_float"), {llvmIncoming});
                    else if (llvmIncoming->getType()->isIntegerTy(1))
                        llvmIncoming = edgeBuilder.CreateCall(runtimeFns_.at("nova_rt_from_bool"), {llvmIncoming});
                } else if (llvmIncoming->getType() == novaValuePtrTy_) {
                    // Target is a raw scalar, incoming is boxed -> unbox it.
                    if (pending.llvmPhi->getType()->isIntegerTy(64))
                        llvmIncoming = edgeBuilder.CreateCall(runtimeFns_.at("nova_rt_to_int"), {llvmIncoming});
                    else if (pending.llvmPhi->getType()->isDoubleTy())
                        llvmIncoming = edgeBuilder.CreateCall(runtimeFns_.at("nova_rt_to_float"), {llvmIncoming});
                    else if (pending.llvmPhi->getType()->isIntegerTy(1))
                        llvmIncoming = edgeBuilder.CreateCall(runtimeFns_.at("nova_rt_to_bool"), {llvmIncoming});
                }
            }
            pending.llvmPhi->addIncoming(llvmIncoming, predBB);
        }
    }
}

// ═══════════════════════════════ operand / constant resolution ═══════════════════════════════

llvm::Value* LLVMBackend::lowerConstant(const nova::ir::ConstantValue& c) {
    switch (c.type.kind) {
        case nova::ir::TypeKind::Int:
            // Concretely-typed Int constant used directly in a numeric fast
            // path context still needs a raw i64 form; lowerNumericBinary
            // calls resolveOperand and expects the native type here.
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*llvmContext_),
                                           std::stoll(c.literal.empty() ? "0" : c.literal), true);
        case nova::ir::TypeKind::Float:
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*llvmContext_),
                                          std::stod(c.literal.empty() ? "0" : c.literal));
        case nova::ir::TypeKind::Bool:
            return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*llvmContext_),
                                           (c.literal == "true" || c.literal == "1") ? 1 : 0);
        case nova::ir::TypeKind::String: {
            llvm::Constant* strConst = builder_->CreateGlobalStringPtr(c.literal, "str");
            return builder_->CreateCall(runtimeFns_.at("nova_rt_const_string"), {strConst});
        }
        case nova::ir::TypeKind::Null:
            return builder_->CreateCall(runtimeFns_.at("nova_rt_const_null"), {});
        case nova::ir::TypeKind::Any:
        default:
            // An Any-typed constant with numeric-looking literal text still
            // needs *some* boxed representation; fall back to boxing it as
            // a string constant (the runtime's canonical "unknown literal"
            // representation) rather than guessing a numeric parse that may
            // not apply.
            {
                llvm::Constant* strConst = builder_->CreateGlobalStringPtr(c.literal, "lit");
                return builder_->CreateCall(runtimeFns_.at("nova_rt_const_string"), {strConst});
            }
    }
}

llvm::Value* LLVMBackend::resolveOperand(const nova::ir::ValuePtr& operand) {
    if (!operand) return nullptr;

    if (operand->kind == nova::ir::ValueKind::Constant) {
        if (auto* c = dynamic_cast<const nova::ir::ConstantValue*>(operand.get())) {
            return lowerConstant(*c);
        }
    }

    auto it = valueMap_.find(operand.get());
    if (it != valueMap_.end()) return it->second;

    // Referenced before it was lowered — this can legitimately happen for
    // a phi's incoming edge from a block later in source order but earlier
    // in the CFG (loop back-edges); Pass C in lowerFunctionBody handles
    // phi operands specially and never calls resolveOperand for an
    // as-yet-unlowered value outside that path. If we get here for a
    // *non*-phi use, it indicates the IR references a value whose defining
    // instruction genuinely has not executed yet in this block ordering —
    // report it rather than dereferencing a missing map entry.
    diag("unresolved IR value '" + operand->ref() + "' (used before its defining instruction)");
    return llvm::UndefValue::get(operand->type.kind == nova::ir::TypeKind::Any ? novaValuePtrTy_
                                                                                 : lowerType(operand->type));
}

// ═══════════════════════════════ instruction lowering ═══════════════════════════════

llvm::Value* LLVMBackend::lowerInstruction(const nova::ir::Instruction& inst,
                                            std::vector<PendingPhi>& pendingPhis) {
    using Op = nova::ir::Opcode;

    switch (inst.opcode) {
        case Op::Nop:
            return nullptr;

        // ---- memory ----
        case Op::Alloc: {
            // nova::ir::IRBuilder never actually emits Alloc today (locals
            // live directly as SSA values), but the opcode is part of the
            // IR's public surface, so it is still handled correctly: a
            // heap slot big enough to hold one boxed NovaValue*.
            llvm::Value* addr = builder_->CreateCall(runtimeFns_.at("nova_rt_alloc"),
                {llvm::ConstantInt::get(llvm::Type::getInt64Ty(*llvmContext_), 8)});
            return addr;
        }
        case Op::Load: {
            llvm::Value* addr = resolveOperand(inst.operands.at(0));
            return builder_->CreateCall(runtimeFns_.at("nova_rt_load"), {addr});
        }
        case Op::Store: {
            llvm::Value* addr = resolveOperand(inst.operands.at(0));
            llvm::Value* val = resolveOperand(inst.operands.at(1));
            builder_->CreateCall(runtimeFns_.at("nova_rt_store"), {addr, val});
            return nullptr;
        }

        // ---- arithmetic / comparison / logic: numeric fast path first ----
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::Pow:
        case Op::Eq:  case Op::Ne:  case Op::Lt:  case Op::Le:  case Op::Gt:  case Op::Ge:
        case Op::And: case Op::Or: {
            bool operandsNumeric = inst.operands.size() == 2 &&
                isNumericType(inst.operands[0]->type) && isNumericType(inst.operands[1]->type);
            if (operandsNumeric) {
                llvm::Value* r = lowerNumericBinary(inst);
                if (r) return r; // fast path succeeded
                // fall through to boxed runtime path if the fast path
                // declined (e.g. mixed Int/Float needing a box-based promote)
            }
            return lowerRuntimeOp(inst);
        }

        case Op::Neg: case Op::Not: {
            if (inst.operands.size() == 1 && isNumericType(inst.operands[0]->type)) {
                llvm::Value* v = resolveOperand(inst.operands[0]);
                if (inst.opcode == Op::Neg) {
                    if (v->getType()->isDoubleTy()) return builder_->CreateFNeg(v, inst.name);
                    if (v->getType()->isIntegerTy()) return builder_->CreateNeg(v, inst.name);
                } else { // Not
                    if (v->getType()->isIntegerTy(1)) return builder_->CreateNot(v, inst.name);
                }
            }
            return lowerRuntimeOp(inst);
        }

        // ---- control flow terminators ----
        case Op::Jump: {
            if (inst.successors.empty()) { diag("Jump with no successor in '" + inst.name + "'"); return nullptr; }
            llvm::BasicBlock* target = blockMap_.at(inst.successors[0]);
            builder_->CreateBr(target);
            return nullptr;
        }
        case Op::CondBranch: {
            if (inst.successors.size() < 2) {
                diag("CondBranch with fewer than 2 successors in '" + inst.name + "'");
                return nullptr;
            }
            llvm::Value* condBoxedOrRaw = resolveOperand(inst.operands.at(0));
            llvm::Value* condI1 = condBoxedOrRaw;
            if (condI1->getType() != llvm::Type::getInt1Ty(*llvmContext_)) {
                condI1 = builder_->CreateCall(runtimeFns_.at("nova_rt_to_bool"), {condBoxedOrRaw});
            }
            llvm::BasicBlock* trueBB = blockMap_.at(inst.successors[0]);
            llvm::BasicBlock* falseBB = blockMap_.at(inst.successors[1]);
            builder_->CreateCondBr(condI1, trueBB, falseBB);
            return nullptr;
        }
        case Op::Return: {
            llvm::Function* fn = builder_->GetInsertBlock()->getParent();
            if (inst.operands.empty()) {
                if (fn->getReturnType()->isVoidTy()) builder_->CreateRetVoid();
                else builder_->CreateRet(llvm::Constant::getNullValue(fn->getReturnType()));
            } else {
                llvm::Value* retVal = resolveOperand(inst.operands[0]);
                if (fn->getReturnType()->isVoidTy()) {
                    builder_->CreateRetVoid();
                } else if (retVal->getType() != fn->getReturnType()) {
                    // Box/unbox to match the declared return type, same
                    // reasoning as the phi-edge patching above.
                    if (fn->getReturnType() == novaValuePtrTy_) {
                        if (retVal->getType()->isIntegerTy(64))
                            retVal = builder_->CreateCall(runtimeFns_.at("nova_rt_from_int"), {retVal});
                        else if (retVal->getType()->isDoubleTy())
                            retVal = builder_->CreateCall(runtimeFns_.at("nova_rt_from_float"), {retVal});
                        else if (retVal->getType()->isIntegerTy(1))
                            retVal = builder_->CreateCall(runtimeFns_.at("nova_rt_from_bool"), {retVal});
                    } else if (retVal->getType() == novaValuePtrTy_) {
                        if (fn->getReturnType()->isIntegerTy(64))
                            retVal = builder_->CreateCall(runtimeFns_.at("nova_rt_to_int"), {retVal});
                        else if (fn->getReturnType()->isDoubleTy())
                            retVal = builder_->CreateCall(runtimeFns_.at("nova_rt_to_float"), {retVal});
                        else if (fn->getReturnType()->isIntegerTy(1))
                            retVal = builder_->CreateCall(runtimeFns_.at("nova_rt_to_bool"), {retVal});
                    }
                    builder_->CreateRet(retVal);
                } else {
                    builder_->CreateRet(retVal);
                }
            }
            return nullptr;
        }
        case Op::Invoke: {
            // nova::ir's Invoke is "call with explicit exceptional
            // successor"; IRBuilder as shipped never emits it (error
            // propagation is modeled as ErrorCheck + CondBranch instead —
            // see IRBuilder::lowerErrorPropagation). Still handled: lower
            // as an ordinary runtime call, then branch to successors[0]
            // (normal) since nova_rt_call reports failure via the boxed
            // result rather than an LLVM-level exception.
            llvm::Value* result = lowerRuntimeOp(inst);
            if (!inst.successors.empty()) builder_->CreateBr(blockMap_.at(inst.successors[0]));
            return result;
        }

        // ---- phi: create now, patch incoming edges in Pass C ----
        case Op::Phi: {
            llvm::Type* phiTy = isNumericType(inst.type) ? lowerType(inst.type) : novaValuePtrTy_;
            llvm::PHINode* phi = builder_->CreatePHI(phiTy, static_cast<unsigned>(inst.operands.size()),
                                                      inst.name.empty() ? "phi" : inst.name);
            pendingPhis.push_back({phi, &inst});
            return phi;
        }

        case Op::Select: {
            llvm::Value* cond = resolveOperand(inst.operands.at(0));
            llvm::Value* a = resolveOperand(inst.operands.at(1));
            llvm::Value* b = resolveOperand(inst.operands.at(2));
            if (cond->getType() != llvm::Type::getInt1Ty(*llvmContext_)) {
                cond = builder_->CreateCall(runtimeFns_.at("nova_rt_to_bool"), {cond});
            }
            if (a->getType() == b->getType()) {
                return builder_->CreateSelect(cond, a, b, inst.name);
            }
            // Mixed native/boxed operands: fall back to the runtime's own
            // select so it can box/normalize both arms uniformly.
            llvm::Value* condBoxed = builder_->CreateCall(runtimeFns_.at("nova_rt_const_bool"), {cond});
            return builder_->CreateCall(runtimeFns_.at("nova_rt_select"), {condBoxed, a, b}, inst.name);
        }

        // ---- everything else: casts, member/index/slice, collection
        //      construction, calls, iteration, async, error handling — all
        //      go through the boxed runtime ABI (see header contract) ----
        case Op::Cast:
        case Op::Index:
        case Op::Member:
        case Op::MakeArray:
        case Op::MakeMap:
        case Op::MakeTuple:
        case Op::Slice:
        case Op::Call:
        case Op::Await:
        case Op::Yield:
        case Op::AsyncSuspend:
        case Op::AsyncResume:
        case Op::ErrorCheck:
        case Op::Runtime:
            return lowerRuntimeOp(inst);
    }

    diag("unhandled opcode in lowerInstruction (opcode value=" +
         std::to_string(static_cast<int>(inst.opcode)) + ")");
    return nullptr;
}

// Native LLVM arithmetic/comparison for two concretely-numeric operands.
// Returns nullptr (declining) only in the mixed-Int/Float-needing-a-box
// case that doesn't arise from IRBuilder's own emission today but is
// handled defensively; every other numeric combination is fully lowered
// here to genuine LLVM instructions, not a runtime call.
llvm::Value* LLVMBackend::lowerNumericBinary(const nova::ir::Instruction& inst) {
    using Op = nova::ir::Opcode;
    llvm::Value* lhs = resolveOperand(inst.operands.at(0));
    llvm::Value* rhs = resolveOperand(inst.operands.at(1));

    bool lhsFloat = lhs->getType()->isDoubleTy();
    bool rhsFloat = rhs->getType()->isDoubleTy();
    if (lhsFloat != rhsFloat) {
        // Int/Float mix: widen the Int side to double (matches Nova's
        // documented numeric-widening rule, Int -> Float, from the type
        // checker) using native LLVM conversion — still the fast path.
        if (!lhsFloat && lhs->getType()->isIntegerTy(64))
            lhs = builder_->CreateSIToFP(lhs, llvm::Type::getDoubleTy(*llvmContext_), "widen");
        if (!rhsFloat && rhs->getType()->isIntegerTy(64))
            rhs = builder_->CreateSIToFP(rhs, llvm::Type::getDoubleTy(*llvmContext_), "widen");
        lhsFloat = rhsFloat = true;
    }

    const std::string& n = inst.name;
    switch (inst.opcode) {
        case Op::Add: return lhsFloat ? builder_->CreateFAdd(lhs, rhs, n) : builder_->CreateAdd(lhs, rhs, n);
        case Op::Sub: return lhsFloat ? builder_->CreateFSub(lhs, rhs, n) : builder_->CreateSub(lhs, rhs, n);
        case Op::Mul: return lhsFloat ? builder_->CreateFMul(lhs, rhs, n) : builder_->CreateMul(lhs, rhs, n);
        case Op::Div: return lhsFloat ? builder_->CreateFDiv(lhs, rhs, n) : builder_->CreateSDiv(lhs, rhs, n);
        case Op::Mod: return lhsFloat ? builder_->CreateFRem(lhs, rhs, n) : builder_->CreateSRem(lhs, rhs, n);
        case Op::Pow:
            // No native LLVM integer/float "pow" instruction — decline the
            // fast path so lowerRuntimeOp's nova_rt_pow handles it
            // (correct for both Int^Int and Float^Float without pulling in
            // llvm.pow intrinsic type restrictions here).
            return nullptr;

        case Op::Eq: return lhsFloat ? builder_->CreateFCmpOEQ(lhs, rhs, n) : builder_->CreateICmpEQ(lhs, rhs, n);
        case Op::Ne: return lhsFloat ? builder_->CreateFCmpONE(lhs, rhs, n) : builder_->CreateICmpNE(lhs, rhs, n);
        case Op::Lt: return lhsFloat ? builder_->CreateFCmpOLT(lhs, rhs, n) : builder_->CreateICmpSLT(lhs, rhs, n);
        case Op::Le: return lhsFloat ? builder_->CreateFCmpOLE(lhs, rhs, n) : builder_->CreateICmpSLE(lhs, rhs, n);
        case Op::Gt: return lhsFloat ? builder_->CreateFCmpOGT(lhs, rhs, n) : builder_->CreateICmpSGT(lhs, rhs, n);
        case Op::Ge: return lhsFloat ? builder_->CreateFCmpOGE(lhs, rhs, n) : builder_->CreateICmpSGE(lhs, rhs, n);

        case Op::And:
            if (lhs->getType()->isIntegerTy(1) && rhs->getType()->isIntegerTy(1))
                return builder_->CreateAnd(lhs, rhs, n);
            return nullptr; // non-bool numeric "and" isn't native-representable; use runtime
        case Op::Or:
            if (lhs->getType()->isIntegerTy(1) && rhs->getType()->isIntegerTy(1))
                return builder_->CreateOr(lhs, rhs, n);
            return nullptr;

        default:
            return nullptr;
    }
}

// Boxed runtime-call fallback used for: any Any-typed binary/unary op, Pow,
// non-bool And/Or, and every "everything else" opcode (Cast/Index/Member/
// MakeArray/MakeMap/MakeTuple/Slice/Call/Await/Yield/AsyncSuspend/
// AsyncResume/ErrorCheck/Runtime). Each operand is boxed on demand if it's
// currently a raw native scalar, so this path is safe to call regardless of
// what the numeric fast path already attempted.
llvm::Value* LLVMBackend::lowerRuntimeOp(const nova::ir::Instruction& inst) {
    using Op = nova::ir::Opcode;

    auto boxIfNeeded = [&](llvm::Value* v) -> llvm::Value* {
        if (!v) return v;
        if (v->getType() == novaValuePtrTy_) return v;
        if (v->getType()->isIntegerTy(64)) return builder_->CreateCall(runtimeFns_.at("nova_rt_from_int"), {v});
        if (v->getType()->isDoubleTy())    return builder_->CreateCall(runtimeFns_.at("nova_rt_from_float"), {v});
        if (v->getType()->isIntegerTy(1))  return builder_->CreateCall(runtimeFns_.at("nova_rt_from_bool"), {v});
        return v; // already some other pointer type (e.g. i8* literal) — pass through
    };

    switch (inst.opcode) {
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::Pow:
        case Op::Eq:  case Op::Ne:  case Op::Lt:  case Op::Le:  case Op::Gt:  case Op::Ge:
        case Op::And: case Op::Or: {
            static const std::unordered_map<Op, const char*> table = {
                {Op::Add,"nova_rt_add"}, {Op::Sub,"nova_rt_sub"}, {Op::Mul,"nova_rt_mul"},
                {Op::Div,"nova_rt_div"}, {Op::Mod,"nova_rt_mod"}, {Op::Pow,"nova_rt_pow"},
                {Op::Eq,"nova_rt_eq"},   {Op::Ne,"nova_rt_ne"},   {Op::Lt,"nova_rt_lt"},
                {Op::Le,"nova_rt_le"},   {Op::Gt,"nova_rt_gt"},   {Op::Ge,"nova_rt_ge"},
                {Op::And,"nova_rt_and"}, {Op::Or,"nova_rt_or"},
            };
            llvm::Value* lhs = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            llvm::Value* rhs = boxIfNeeded(resolveOperand(inst.operands.at(1)));
            return builder_->CreateCall(runtimeFns_.at(table.at(inst.opcode)), {lhs, rhs}, inst.name);
        }
        case Op::Neg: {
            llvm::Value* v = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            return builder_->CreateCall(runtimeFns_.at("nova_rt_neg"), {v}, inst.name);
        }
        case Op::Not: {
            llvm::Value* v = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            return builder_->CreateCall(runtimeFns_.at("nova_rt_not"), {v}, inst.name);
        }

        case Op::Cast: {
            llvm::Value* v = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            std::string typeName = inst.attributes.count("type") ? inst.attributes.at("type") : inst.type.name;
            llvm::Constant* nameConst = builder_->CreateGlobalStringPtr(typeName, "castty");
            return builder_->CreateCall(runtimeFns_.at("nova_rt_cast"), {v, nameConst}, inst.name);
        }
        case Op::Index: {
            llvm::Value* base = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            llvm::Value* key = boxIfNeeded(resolveOperand(inst.operands.at(1)));
            return builder_->CreateCall(runtimeFns_.at("nova_rt_index"), {base, key}, inst.name);
        }
        case Op::Member: {
            llvm::Value* base = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            llvm::Constant* nameConst = builder_->CreateGlobalStringPtr(inst.name.empty() ? "" : inst.name, "member");
            return builder_->CreateCall(runtimeFns_.at("nova_rt_member"), {base, nameConst}, inst.name);
        }
        case Op::Slice: {
            llvm::Value* base = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            llvm::Value* start = inst.operands.size() > 1 ? boxIfNeeded(resolveOperand(inst.operands[1]))
                                                            : builder_->CreateCall(runtimeFns_.at("nova_rt_const_null"), {});
            llvm::Value* end = inst.operands.size() > 2 ? boxIfNeeded(resolveOperand(inst.operands[2]))
                                                          : builder_->CreateCall(runtimeFns_.at("nova_rt_const_null"), {});
            llvm::Value* step = inst.operands.size() > 3 ? boxIfNeeded(resolveOperand(inst.operands[3]))
                                                           : builder_->CreateCall(runtimeFns_.at("nova_rt_const_null"), {});
            return builder_->CreateCall(runtimeFns_.at("nova_rt_slice"), {base, start, end, step}, inst.name);
        }
        case Op::MakeArray:
        case Op::MakeMap:
        case Op::MakeTuple: {
            const char* fnName = inst.opcode == Op::MakeArray ? "nova_rt_make_array"
                                : inst.opcode == Op::MakeMap   ? "nova_rt_make_map"
                                                                 : "nova_rt_make_tuple";
            std::size_t count = inst.operands.size();
            llvm::Value* countConst = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*llvmContext_), count);
            if (count == 0) {
                llvm::Value* nullArr = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(novaValuePtrTy_));
                return builder_->CreateCall(runtimeFns_.at(fnName), {nullArr, countConst}, inst.name);
            }
            // Materialize a stack array of NovaValue* and pass its decayed
            // pointer — the standard "array of boxed elements" ABI shape.
            llvm::ArrayType* arrTy = llvm::ArrayType::get(novaValuePtrTy_, count);
            llvm::Value* arrAlloca = builder_->CreateAlloca(arrTy, nullptr, inst.name + ".elems");
            for (std::size_t i = 0; i < count; ++i) {
                llvm::Value* elem = boxIfNeeded(resolveOperand(inst.operands[i]));
                llvm::Value* slot = builder_->CreateInBoundsGEP(
                    arrTy, arrAlloca,
                    {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvmContext_), 0),
                     llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvmContext_), i)});
                builder_->CreateStore(elem, slot);
            }
            llvm::Value* decayed = builder_->CreateInBoundsGEP(
                arrTy, arrAlloca,
                {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvmContext_), 0),
                 llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvmContext_), 0)});
            return builder_->CreateCall(runtimeFns_.at(fnName), {decayed, countConst}, inst.name);
        }

        case Op::ErrorCheck: {
            llvm::Value* v = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            return builder_->CreateCall(runtimeFns_.at("nova_rt_error_check"), {v}, inst.name);
        }
        case Op::Await: {
            llvm::Value* v = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            return builder_->CreateCall(runtimeFns_.at("nova_rt_await"), {v}, inst.name);
        }
        case Op::Yield: {
            llvm::Value* v = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            return builder_->CreateCall(runtimeFns_.at("nova_rt_yield"), {v}, inst.name);
        }
        case Op::AsyncSuspend: {
            llvm::Value* v = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            builder_->CreateCall(runtimeFns_.at("nova_rt_async_suspend"), {v});
            return nullptr;
        }
        case Op::AsyncResume: {
            llvm::Value* v = boxIfNeeded(resolveOperand(inst.operands.at(0)));
            builder_->CreateCall(runtimeFns_.at("nova_rt_async_resume"), {v});
            return nullptr;
        }

        case Op::Call:
        case Op::Invoke:
        case Op::Runtime: {
            // Per the header contract: IRBuilder encodes EVERY call —
            // ordinary Nova calls, method calls, indirect calls, and every
            // "nova.*" runtime hook (iteration, error handling, threading,
            // signals, channels, comprehensions, casts routed generically,
            // ...) — as Opcode::Runtime whose FIRST operand is a
            // ConstantValue<String> naming the target, followed by the
            // actual arguments (see IRBuilder::runtimeCall /
            // IRBuilder::lowerCall). We therefore dispatch everything here
            // through the single generic nova_rt_call(name, args, argc)
            // entry point, which the runtime resolves by name at call time.
            //
            // Opcode::Call itself is not emitted by IRBuilder as shipped,
            // but is handled identically here for forward-compatibility
            // with a future direct-call-emitting version of IRBuilder.
            if (inst.operands.empty()) {
                diag("Runtime/Call instruction '" + inst.name + "' has no operands (missing callee name)");
                return llvm::ConstantPointerNull::get(novaValuePtrTy_);
            }
            std::string calleeName = inst.name; // runtimeCall() passes `name` as both operand[0] AND the hint/name
            std::size_t argStart = 1;
            // Defensive: if operand[0] isn't actually the name constant
            // (e.g. a hand-built Runtime instruction without the
            // convention), fall back to using ALL operands as args and the
            // instruction's own `name` field as the callee.
            if (auto* c = dynamic_cast<const nova::ir::ConstantValue*>(inst.operands[0].get());
                c && c->type.kind == nova::ir::TypeKind::String) {
                calleeName = c->literal;
            } else {
                argStart = 0;
            }

            std::vector<llvm::Value*> argValues;
            for (std::size_t i = argStart; i < inst.operands.size(); ++i) {
                argValues.push_back(boxIfNeeded(resolveOperand(inst.operands[i])));
            }

            llvm::Constant* nameConst = builder_->CreateGlobalStringPtr(calleeName, "callee");
            llvm::Value* argc = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*llvmContext_), argValues.size());

            llvm::Value* argsPtr;
            if (argValues.empty()) {
                argsPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(novaValuePtrTy_));
            } else {
                llvm::ArrayType* arrTy = llvm::ArrayType::get(novaValuePtrTy_, argValues.size());
                llvm::Value* arrAlloca = builder_->CreateAlloca(arrTy, nullptr, "callargs");
                for (std::size_t i = 0; i < argValues.size(); ++i) {
                    llvm::Value* slot = builder_->CreateInBoundsGEP(
                        arrTy, arrAlloca,
                        {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvmContext_), 0),
                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvmContext_), i)});
                    builder_->CreateStore(argValues[i], slot);
                }
                argsPtr = builder_->CreateInBoundsGEP(
                    arrTy, arrAlloca,
                    {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvmContext_), 0),
                     llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvmContext_), 0)});
            }
            return builder_->CreateCall(runtimeFns_.at("nova_rt_call"), {nameConst, argsPtr, argc},
                                         inst.name.empty() ? "calltmp" : inst.name);
        }

        default:
            diag("lowerRuntimeOp: unhandled opcode (value=" + std::to_string(static_cast<int>(inst.opcode)) + ")");
            return llvm::ConstantPointerNull::get(novaValuePtrTy_);
    }
}

// ═══════════════════════════════ textual IR + object code emission ═══════════════════════════════

bool LLVMBackend::emitTextualIR(const std::string& filename) const {
    std::error_code ec;
    llvm::raw_fd_ostream out(filename, ec, llvm::sys::fs::OF_Text);
    if (ec) {
        const_cast<LLVMBackend*>(this)->diag("emitTextualIR: failed to open '" + filename + "': " + ec.message());
        return false;
    }
    llvmModule_->print(out, nullptr);
    return true;
}

bool LLVMBackend::emitObjectCode(const std::string& filename) {
    // ---- 1. initialize LLVM's native target backends ----
    // Registering All* rather than only Native* keeps cross-compilation
    // (BackendOptions::targetTriple set to a non-host triple) working
    // without a second code path; the extra registration cost is
    // negligible for an offline ahead-of-time compiler like novac.
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    std::string triple = options_.targetTriple.empty()
        ? llvm::sys::getDefaultTargetTriple()
        : options_.targetTriple;
    llvmModule_->setTargetTriple(llvm::Triple(triple));

    std::string lookupErr;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupErr);
    if (!target) {
        diag("emitObjectCode: could not find target for triple '" + triple + "': " + lookupErr);
        return false;
    }

    llvm::TargetOptions targetOptions;
    std::optional<llvm::Reloc::Model> relocModel = llvm::Reloc::PIC_;

    std::string cpu = options_.cpu;
    if (cpu == "native") cpu = std::string(llvm::sys::getHostCPUName());

    std::unique_ptr<llvm::TargetMachine> targetMachine(target->createTargetMachine(
        triple, cpu, options_.cpuFeatures, targetOptions, relocModel));
    if (!targetMachine) {
        diag("emitObjectCode: failed to create TargetMachine for triple '" + triple + "'");
        return false;
    }

    llvmModule_->setDataLayout(targetMachine->createDataLayout());

    // ---- 2. run the optimization pipeline (new pass manager) ----
    // Kept intentionally simple and correct rather than maximally tuned:
    // the standard -O<n> default pipeline via PassBuilder, which already
    // includes mem2reg, inlining, DCE, etc. appropriate to optLevel. A
    // dedicated Nova-specific pass (e.g. specializing nova_rt_call sites
    // whose callee name is a compile-time constant) is a natural future
    // addition here, not a requirement for a correct first backend.
    {
        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;

        llvm::PassBuilder PB(targetMachine.get());
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

        llvm::OptimizationLevel level;
        switch (options_.optLevel) {
            case 0:  level = llvm::OptimizationLevel::O0; break;
            case 1:  level = llvm::OptimizationLevel::O1; break;
            case 2:  level = llvm::OptimizationLevel::O2; break;
            default: level = llvm::OptimizationLevel::O3; break;
        }

        llvm::ModulePassManager MPM;
        if (level == llvm::OptimizationLevel::O0) {
            MPM = PB.buildO0DefaultPipeline(level);
        } else {
            MPM = PB.buildPerModuleDefaultPipeline(level);
        }
        MPM.run(*llvmModule_, MAM);
    }

    // Re-verify post-optimization: a passing pre-opt verify combined with a
    // failing post-opt verify would indicate a real LLVM/pass-pipeline bug,
    // and is far better caught here than as a silent miscompile.
    std::string postOptErr;
    llvm::raw_string_ostream postOptOs(postOptErr);
    if (llvm::verifyModule(*llvmModule_, &postOptOs)) {
        diag("post-optimization module verification failed:\n" + postOptOs.str());
        return false;
    }

    if (options_.emitTextualIR) {
        std::string path = options_.textualIRPath.empty() ? (filename + ".ll") : options_.textualIRPath;
        emitTextualIR(path);
    }

    // ---- 3. emit the object file ----
    std::error_code ec;
    llvm::raw_fd_ostream dest(filename, ec, llvm::sys::fs::OF_None);
    if (ec) {
        diag("emitObjectCode: could not open output file '" + filename + "': " + ec.message());
        return false;
    }

    llvm::legacy::PassManager codegenPM;
    if (targetMachine->addPassesToEmitFile(codegenPM, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        diag("emitObjectCode: target machine cannot emit object files for triple '" + triple + "'");
        return false;
    }
    codegenPM.run(*llvmModule_);
    dest.flush();

    return true;
}

} // namespace nova::backend
