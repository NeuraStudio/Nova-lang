// LLVMBackend.hpp — Nova IR -> LLVM IR -> native object code.
//
// ═══════════════════════════════════════════════════════════════════════
// IMPORTANT — read before wiring this into the build:
//
// nova::ir is a dynamically-typed, runtime-call-based SSA IR (see IR.hpp /
// IRBuilder.hpp/.cpp as actually shipped in this project). Concretely:
//
//   - Every value's nova::ir::Type is overwhelmingly TypeKind::Any at
//     lowering time. IRBuilder does not emit Opcode::Alloc or Opcode::Call;
//     essentially all "real work" (arithmetic on unknown-typed operands,
//     member/index access, iteration, calls, casts, comprehensions, thread
//     spawn, signals, channels, error propagation, await...) is expressed
//     as Opcode::Runtime instructions whose first operand is a
//     ConstantValue<String> naming a runtime entry point (e.g.
//     "nova.call", "nova.iter.begin", "nova.error.extract", ...).
//   - Opcode::Add/Sub/Mul/... and Opcode::Eq/Lt/... DO appear directly for
//     values IRBuilder can prove are Int/Float/Bool at IR-construction time
//     (see the binary-expr lowering in IRBuilder.cpp), but any operand that
//     is Any must still be represented as a boxed runtime value at the LLVM
//     level — you cannot emit a native `fadd`/`add` on it without knowing
//     its runtime type first.
//
// Consequently, this backend does NOT attempt to unbox everything down to
// native i64/double registers (that is a future, separate "narrowing" /
// specialization pass over nova::ir — a legitimate next Map, not this one).
// Instead it lowers every nova::ir::Value to a single opaque pointer type,
// `%NovaValue*` (an LLVM alias for i8*), and every opcode to a call into a
// small, explicit C ABI implemented by the Nova runtime (nova_rt.cpp,
// already sketched elsewhere in this project). This is the same strategy
// production dynamic-language backends use (e.g. early-stage CPython/JS
// JITs before type specialization): correct, always compiles, and gives a
// stable seam for a later optimizing pass to replace individual Runtime
// calls with native instructions once operand types are actually known.
//
// Arithmetic/comparison opcodes that DO carry concrete Int/Float/Bool
// types on both operands ARE lowered to genuine native LLVM instructions
// (integer/float add/sub/mul/div, icmp/fcmp) rather than a runtime call —
// see lowerInstruction()'s numeric fast path. Everything else — including
// any Any-typed arithmetic — goes through the boxed runtime ABI so it is
// always correct regardless of the runtime value's actual dynamic type.
//
// Runtime ABI assumed (all `NovaValue` = opaque `i8*`; declared as LLVM
// external functions, resolved at link time against nova_rt.cpp/.o):
//
//   NovaValue* nova_rt_call(const char* name, NovaValue** args, i64 argc);
//   NovaValue* nova_rt_alloc(i64 size);
//   NovaValue* nova_rt_load(NovaValue* addr);
//   void       nova_rt_store(NovaValue* addr, NovaValue* value);
//   NovaValue* nova_rt_const_int(i64 v);
//   NovaValue* nova_rt_const_float(double v);
//   NovaValue* nova_rt_const_string(const char* v);
//   NovaValue* nova_rt_const_bool(i1 v);
//   NovaValue* nova_rt_const_null(void);
//   i64        nova_rt_to_int(NovaValue*);
//   double     nova_rt_to_float(NovaValue*);
//   i1         nova_rt_to_bool(NovaValue*);
//   NovaValue* nova_rt_from_int(i64);
//   NovaValue* nova_rt_from_float(double);
//   NovaValue* nova_rt_from_bool(i1);
//   NovaValue* nova_rt_add/sub/mul/div/mod/pow(NovaValue*, NovaValue*);
//   NovaValue* nova_rt_eq/ne/lt/le/gt/ge(NovaValue*, NovaValue*); // returns boxed Bool
//   NovaValue* nova_rt_and/or(NovaValue*, NovaValue*);
//   NovaValue* nova_rt_neg/not(NovaValue*);
//   NovaValue* nova_rt_select(NovaValue* cond, NovaValue* a, NovaValue* b);
//   NovaValue* nova_rt_cast(NovaValue*, const char* typeName);
//   NovaValue* nova_rt_index(NovaValue*, NovaValue* key);
//   NovaValue* nova_rt_member(NovaValue*, const char* name);
//   NovaValue* nova_rt_slice(NovaValue*, NovaValue* start, NovaValue* end, NovaValue* step);
//   NovaValue* nova_rt_make_array(NovaValue** elems, i64 count);
//   NovaValue* nova_rt_make_map(NovaValue** kvPairs, i64 pairCount); // interleaved k,v
//   NovaValue* nova_rt_make_tuple(NovaValue** elems, i64 count);
//   i1         nova_rt_error_check(NovaValue*);
//   NovaValue* nova_rt_await(NovaValue*);
//   NovaValue* nova_rt_yield(NovaValue*);
//   void       nova_rt_async_suspend(NovaValue*);
//   void       nova_rt_async_resume(NovaValue*);
//   i32        nova_main(void);   // emitted as the object file's entry hook
//
// The header above is the CONTRACT this backend compiles against. It must
// match nova_rt.cpp's actual exported symbols (extern "C") exactly, or the
// object file will link but crash / fail to link with undefined symbols.
// ═══════════════════════════════════════════════════════════════════════

#pragma once

#include "IR.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nova::backend {

// Target triple / codegen preferences for emitObjectCode(). Left as plain
// strings/enums (not llvm::Triple/CodeGenOpt directly) so this header does
// not have to pull in the full Target/TargetMachine headers — those are
// only needed in the .cpp, where the concrete llvm::TargetMachine is built.
struct BackendOptions {
    // Empty string = use LLVM's host-detected default triple
    // (llvm::sys::getDefaultTargetTriple()). Set explicitly for
    // cross-compilation, e.g. "aarch64-linux-gnu" or "x86_64-pc-linux-gnu".
    std::string targetTriple;

    // "generic" is always safe. Set to the host CPU name (or "native" —
    // resolved via llvm::sys::getHostCPUName() in the .cpp) for
    // performance-sensitive local builds.
    std::string cpu = "generic";
    std::string cpuFeatures;

    // 0 = -O0 (fastest to build, easiest to debug the generated IR),
    // 1 = -O1, 2 = -O2, 3 = -O3. Passed straight into the new-pass-manager
    // pipeline builder in emitObjectCode().
    unsigned optLevel = 1;

    // Emit a human-readable *.ll file alongside the object file, useful
    // while nova_rt.cpp's ABI is still being iterated on.
    bool emitTextualIR = false;
    std::string textualIRPath; // defaults to <objectFilename>.ll if empty
};

// Lowers one nova::ir::Module into one llvm::Module and can emit that as a
// native object file. One LLVMBackend instance is meant for one nova::ir
// Module — construct a fresh instance per compilation unit; LLVMContext
// ownership is entirely internal so there is no cross-instance sharing to
// reason about.
class LLVMBackend {
public:
    explicit LLVMBackend(BackendOptions options = {});
    ~LLVMBackend();

    LLVMBackend(const LLVMBackend&) = delete;
    LLVMBackend& operator=(const LLVMBackend&) = delete;

    // Performs the full nova::ir::Module -> llvm::Module lowering. Must be
    // called exactly once before emitObjectCode()/emitTextualIR() /
    // takeModule(). Returns false and leaves diagnostics() populated if
    // lowering hit a structural problem it could not recover from (e.g. a
    // branch to a block that was never created) — such problems indicate a
    // bug in IRBuilder's SSA construction, not a normal compile error, so
    // they are reported rather than silently patched over.
    bool lower(const nova::ir::Module& novaModule);

    // Runs the LLVM optimization pipeline (per BackendOptions::optLevel),
    // verifies the module, initializes the requested target, and writes a
    // native object file to `filename`. Returns false on failure (check
    // diagnostics()). Safe to call only after a successful lower().
    bool emitObjectCode(const std::string& filename);

    // Writes the current LLVM IR (post-lower, pre- or post-optimization
    // depending on when it's called) as textual .ll to `filename`. Useful
    // standalone for debugging without going all the way to an object file.
    bool emitTextualIR(const std::string& filename) const;

    // Accumulated warnings/errors from lower()/emitObjectCode(). Non-empty
    // does not necessarily mean failure — check the bool return values.
    const std::vector<std::string>& diagnostics() const { return diagnostics_; }

    // Read-only access to the underlying llvm::Module, e.g. for embedding
    // this backend inside a JIT driver instead of emitting an object file.
    const llvm::Module* module() const { return llvmModule_.get(); }
    llvm::Module* module() { return llvmModule_.get(); }

    // Releases ownership of the built llvm::Module (and its LLVMContext,
    // bundled together since an llvm::Module can't outlive its Context).
    // After calling this, `module()` returns nullptr and this backend
    // instance must not be used again except for diagnostics().
    std::pair<std::unique_ptr<llvm::LLVMContext>, std::unique_ptr<llvm::Module>> takeModule();

private:
    BackendOptions options_;
    std::unique_ptr<llvm::LLVMContext> llvmContext_;
    std::unique_ptr<llvm::Module> llvmModule_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    std::vector<std::string> diagnostics_;

    // ---- opaque NovaValue type + runtime ABI declarations ----
    // %NovaValue = type opaque ; all boxed Nova values are NovaValue*.
    llvm::StructType* novaValueOpaqueTy_ = nullptr;
    llvm::PointerType* novaValuePtrTy_ = nullptr;

    // Cache of declared runtime functions (name -> llvm::FunctionCallee),
    // populated lazily by declareRuntimeFn() so the header list at the top
    // of this file and the actual declared set never drift apart silently
    // — every runtime symbol used by lowerInstruction() is declared through
    // this single path.
    std::unordered_map<std::string, llvm::FunctionCallee> runtimeFns_;

    // nova::ir::Value* -> llvm::Value* for the function currently being
    // lowered. Cleared and rebuilt per-function (SSA values from IR do not
    // cross function boundaries in nova::ir, matching normal LLVM function
    // isolation).
    std::unordered_map<const nova::ir::Value*, llvm::Value*> valueMap_;

    // nova::ir::BasicBlock* -> llvm::BasicBlock* for the function currently
    // being lowered. Populated in a first pass over all blocks (so forward
    // branches / loop back-edges resolve correctly) before any instruction
    // is lowered.
    std::unordered_map<const nova::ir::BasicBlock*, llvm::BasicBlock*> blockMap_;

    // nova::ir::Function* -> llvm::Function* across the WHOLE module, so
    // that inter-function references (recursive/mutually-recursive calls
    // via Opcode::Runtime "nova.call", which resolves callees dynamically
    // by name at runtime rather than a direct LLVM call — see notes above)
    // still have a place to look up declared native signatures if a later
    // pass wants to add a direct-call fast path.
    std::unordered_map<std::string, llvm::Function*> functionMap_;

    // Deferred phi-node patch list: (llvm::PHINode*, nova::ir Instruction*)
    // pairs collected while lowering a block, resolved once every block in
    // the function has an llvm::BasicBlock* (so a phi incoming from a
    // not-yet-lowered predecessor still resolves correctly). This is the
    // standard two-pass approach for lowering an already-SSA IR into LLVM.
    struct PendingPhi {
        llvm::PHINode* llvmPhi;
        const nova::ir::Instruction* novaPhi;
    };

    // ---- type lowering ----
    llvm::Type* lowerType(const nova::ir::Type& t);

    // ---- module-level lowering ----
    void declareOpaqueNovaValueType();
    llvm::FunctionCallee declareRuntimeFn(const std::string& name,
                                           llvm::Type* returnTy,
                                           std::vector<llvm::Type*> paramTys,
                                           bool isVarArg = false);
    void declareAllRuntimeFns();
    llvm::Function* declareNovaFunction(const nova::ir::Function& fn);
    void lowerFunctionBody(const nova::ir::Function& novaFn, llvm::Function* llvmFn);

    // ---- per-instruction lowering ----
    // Returns the llvm::Value* produced by this instruction (or nullptr for
    // void-producing instructions such as Store/Jump/CondBranch/Return).
    llvm::Value* lowerInstruction(const nova::ir::Instruction& inst,
                                   std::vector<PendingPhi>& pendingPhis);

    // Numeric fast path: both operands are already concretely Int/Float/Bool
    // per nova::ir::Type, so this emits genuine native LLVM arithmetic/
    // comparison instructions instead of a boxed runtime call.
    llvm::Value* lowerNumericBinary(const nova::ir::Instruction& inst);
    bool isNumericType(const nova::ir::Type& t) const;

    // Generic fallback: box operands as needed and call the matching
    // nova_rt_* runtime entry point. Used for every Any-typed operation and
    // for every Opcode::Runtime instruction (member/index/call/iteration/
    // await/yield/error-propagation/collection construction/...).
    llvm::Value* lowerRuntimeOp(const nova::ir::Instruction& inst);

    // Resolves a nova::ir::ValuePtr operand to an llvm::Value*, materializing
    // constants (ConstantValue) on demand via the nova_rt_const_* family.
    llvm::Value* resolveOperand(const nova::ir::ValuePtr& operand);
    llvm::Value* lowerConstant(const nova::ir::ConstantValue& c);

    void diag(const std::string& message);
};

} // namespace nova::backend
