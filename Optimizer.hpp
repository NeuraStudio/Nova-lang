// Optimizer.hpp — Map 7: Performance Engine, IR-level half.
//
// ═══════════════════════════════════════════════════════════════════════
// This header declares nova::ir::Optimizer, a pre-LLVM optimization pass
// that runs directly over nova::ir::Module — the SSA IR produced by
// IRBuilder, *before* LLVMBackend ever lowers it. Two classical passes are
// implemented here, matched to what this specific IR actually looks like
// (see IR.hpp / IRBuilder.cpp):
//
//   1. Constant Folding & Propagation
//      IRBuilder already emits genuine Opcode::Add/Sub/Mul/.../Eq/Lt/...
//      instructions (not runtime calls) whenever both operands are
//      statically known to be Int/Float/Bool — see the numeric fast path
//      noted in LLVMBackend.hpp. When *both* operands of such an
//      instruction also happen to be ConstantValue, the whole instruction
//      is redundant: its result is known at compile time. This pass
//      evaluates it and rewrites every use of the instruction's result to
//      point directly at the folded ConstantValue instead, then leaves the
//      now-unused instruction for DCE to remove. Because Nova constants
//      are stored as decimal-text strings (see ConstantValue::literal /
//      IRBuilder::makeConstant), folding parses that text, computes in the
//      matching native C++ type (std::int64_t / double / bool), and
//      re-serializes with the exact same textual conventions IRBuilder
//      itself uses (plain integers, std::setprecision(17) for floats,
//      "true"/"false" for bools) so folded and IRBuilder-emitted constants
//      are textually indistinguishable.
//
//   2. Dead Code Elimination (DCE)
//      Iteratively removes instructions whose SSA result is never read by
//      any other instruction (as an operand) and is never referenced by a
//      Phi's incoming list, EXCLUDING:
//        - terminators (Jump/CondBranch/Return/Invoke) — control flow must
//          never be deleted even though isTerminator() instructions are
//          void-typed and "unused" by definition;
//        - Opcode::Runtime and Opcode::Call — calls into the Nova runtime
//          may have side effects (I/O, mutation, thread/channel/signal
//          operations, error propagation state) that are not visible as
//          SSA def-use edges, so they are conservatively kept even when
//          their result value is unused;
//        - Opcode::Store — writes through a pointer are a side effect by
//          construction, even though Store is void-typed;
//        - Opcode::Invoke / Await / Yield / AsyncSuspend / AsyncResume /
//          ErrorCheck — all carry or gate control/async-state side effects.
//      Because this is SSA, "dead" is purely a liveness question over the
//      use-def graph; no dataflow fixpoint beyond a worklist is needed —
//      removing one dead instruction can only ever make its operands'
//      users-count drop, never rise, so a simple iterate-to-fixpoint loop
//      over a use-count map terminates and is correct.
//
// Both passes operate function-by-function and block-by-block in place,
// mutating the nova::ir::Module passed to Optimizer::run(). They preserve
// the CFG (they never delete blocks or change successors/predecessors,
// nor do they alter Instruction::successors on any terminator) and they
// preserve SSA form (every use is rewritten before the def it referenced
// is ever erased, so the module remains fully consistent at every
// intermediate step, not just at the end of run()).
// ═══════════════════════════════════════════════════════════════════════

#pragma once

#include "IR.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nova::ir {

// Diagnostics / statistics from one Optimizer::run() invocation. Purely
// informational — never affects correctness — but useful for golden tests
// ("did we actually fold what we expected to?") and for CLI -opt-stats
// style flags.
struct OptimizerStats {
    std::uint64_t instructionsFolded = 0;   // constant-folded instructions
    std::uint64_t instructionsRemoved = 0;  // instructions removed by DCE
    std::uint64_t dceIterations = 0;        // fixpoint iterations DCE took

    std::string str() const;
};

// Options controlling which sub-passes run and how aggressively. Default
// values run the full pipeline (fold, then DCE, repeated until neither
// pass makes further progress) — the configuration LLVMBackend should use
// for any optLevel >= 1, mirroring how PerformanceEngine treats optLevel.
struct OptimizerOptions {
    bool enableConstantFolding = true;
    bool enableDeadCodeElimination = true;

    // Constant folding and DCE interact: folding an instruction to a
    // constant can make its (now-unused) operands dead, and removing dead
    // instructions can expose further foldable instructions whose operand
    // list changed. Re-running both passes until a full round makes no
    // further progress (bounded by maxIterations to guarantee termination
    // even under a pathological input) extracts strictly more than a
    // single fold-then-DCE pass would.
    unsigned maxIterations = 8;
};

// Pre-LLVM optimizer over nova::ir::Module. Stateless across calls to
// run() — construct once, call run() for each Module you want optimized
// (or reuse the same instance repeatedly; it carries no mutable state
// between invocations besides the last run's stats, exposed via stats()).
class Optimizer {
public:
    explicit Optimizer(OptimizerOptions options = {});

    // Optimizes `module` in place. Returns true if any change was made
    // (useful for a fixpoint driver one level up that also runs LLVM-side
    // passes and wants to know whether another Nova-IR round is worthwhile).
    bool run(Module& module);

    const OptimizerStats& stats() const { return stats_; }

private:
    OptimizerOptions options_;
    OptimizerStats stats_;

    // Runs constant folding over every instruction in `fn`, rewriting uses
    // of any instruction it folds to point at the new ConstantValue.
    // Returns true if it folded at least one instruction.
    bool foldConstantsInFunction(Function& fn);

    // Attempts to fold a single instruction given its (already-resolved)
    // operands. Returns the folded constant, or nullptr if `inst` is not a
    // foldable opcode or its operands are not both compile-time constants.
    ValuePtr tryFoldInstruction(const Instruction& inst) const;

    // Removes instructions in `fn` whose result is provably unused,
    // excluding terminators and side-effecting opcodes (see file header).
    // Iterates to a fixpoint (bounded by options_.maxIterations). Returns
    // true if it removed at least one instruction.
    bool eliminateDeadCodeInFunction(Function& fn);

    // Rewrites every operand/phi-incoming reference to `from` (matched by
    // ValueId) so it points at `to` instead, across the whole function.
    void replaceAllUsesInFunction(Function& fn, ValueId from, const ValuePtr& to);

    // True for opcodes DCE must never remove even when their SSA result
    // (if any) has zero uses — see file header for the rationale behind
    // each entry.
    static bool hasSideEffectsOrIsTerminator(const Instruction& inst);

    // True for opcodes constant folding knows how to evaluate. Kept
    // separate from the actual evaluation logic so the "is this opcode in
    // scope at all" question and the "what does folding it compute" question
    // don't have to be re-derived at every call site.
    static bool isFoldableOpcode(Opcode op);
};

} // namespace nova::ir

// ═══════════════════════════════════════════════════════════════════════
// nova::backend::PerformanceEngine — LLVM-side aggressive tuning.
//
// A companion utility, in a separate namespace as required, that
// configures LLVM's new PassBuilder / PassManager pipeline far more
// aggressively than LLVMBackend::emitObjectCode()'s current
// buildPerModuleDefaultPipeline(level) call. Where the existing backend
// takes the standard -O<n> default pipeline as-is, PerformanceEngine
// additionally:
//
//   - Forces PassBuilder's loop-unrolling, loop-vectorization, and
//     SLP-vectorization pipeline options on regardless of optLevel
//     (subject to a minimum optLevel gate — see PerformanceOptions),
//     since PassBuilder's O1 pipeline does not run the vectorizers by
//     default and even at O2/O3 some of these are tunable knobs rather
//     than unconditionally-on.
//   - Builds a llvm::TargetMachine tuned for the *host* CPU
//     (-march=native equivalent: llvm::sys::getHostCPUName() plus the
//     host's full feature string from llvm::sys::getHostCPUFeatures(),
//     which is how the AVX/AVX2/AVX-512/SSE4-family feature bits actually
//     get enabled — cpu="native" alone is not sufficient without also
//     forwarding the feature string).
//   - Sets TargetMachine/TargetOptions & PassBuilder::PipelineTuningOptions
//     fields governing inlining threshold, alias-analysis-driven
//     optimizations (basic-aa/tbaa/scev-aa all run as part of the default
//     analysis registration this class performs), and devirtualization
//     iteration count (PassBuilder re-runs the CGSCC inliner pipeline up
//     to MaxDevirtIterations times to chase virtual calls that become
//     devirtualizable only after earlier inlining — this is exposed and
//     forced up from PassBuilder's conservative default).
//
// This class does not itself replace LLVMBackend::emitObjectCode(); it is
// meant to be called from it (or from a call site that already has a
// llvm::Module + llvm::TargetMachine, e.g. after LLVMBackend::lower() and
// LLVMBackend::takeModule()) so LLVMBackend's existing, simpler pipeline
// remains available for -O0/-O1 debug builds and this one is opted into
// for release/performance builds.
// ═══════════════════════════════════════════════════════════════════════

#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <string>

namespace nova::backend {

struct PerformanceOptions {
    // Baseline optimization level fed to PassBuilder as the starting
    // pipeline (O2 or O3 recommended; O0/O1 skip most of what this class
    // exists to force on, see minVectorizeLevel below).
    llvm::OptimizationLevel baseLevel = llvm::OptimizationLevel::O3;

    // ---- host targeting (-march=native) ----
    // When true, the TargetMachine built by createHostTargetMachine() uses
    // llvm::sys::getHostCPUName() and llvm::sys::getHostCPUFeatures() so
    // the compiled object exploits every ISA extension the *building*
    // machine actually has (AVX2/AVX-512/BMI2/FMA/...), matching what
    // `-march=native` does for clang/gcc. Set false for reproducible /
    // portable builds targeting a generic baseline CPU instead.
    bool useNativeCpu = true;

    // Explicit override; if non-empty, used verbatim instead of the
    // host-detected triple. Empty = llvm::sys::getDefaultTargetTriple().
    std::string targetTriple;

    // ---- loop unrolling ----
    bool forceLoopUnrolling = true;
    // Passed straight to PipelineTuningOptions::LoopUnrolling plus (when
    // set) UnrollAggressive-style threshold hints via TargetMachine's
    // -unroll-threshold codegen option string; 0 = let LLVM pick its own
    // (already-aggressive-at-O3) heuristic threshold.
    unsigned unrollThreshold = 0;

    // ---- vectorization ----
    bool enableLoopVectorization = true;
    bool enableSLPVectorization = true;
    // PassBuilder only wires the vectorizers into the pipeline it builds
    // when the OptimizationLevel it's given is >= this. Forcing
    // baseLevel down to O1 while still wanting vectorization would be
    // contradictory, so this is enforced as a floor, not a suggestion:
    // run() clamps the effective level up to this floor whenever either
    // vectorizer flag is on.
    llvm::OptimizationLevel minVectorizeLevel = llvm::OptimizationLevel::O2;

    // ---- inlining / devirtualization ----
    bool aggressiveInlining = true;
    // Forwarded to PipelineTuningOptions::InlinerThreshold when > 0;
    // <= 0 leaves PassBuilder's per-level default threshold untouched
    // (already generous at O3 — this is for going further still).
    int inlineThreshold = 275;
    // PassBuilder re-invokes its CGSCC inline pipeline this many extra
    // times to chase devirtualization opportunities that appear only
    // after a previous inlining round removed an indirection.
    unsigned maxDevirtIterations = 4;

    // ---- alias / escape analysis ----
    // These do not correspond to a single PassBuilder toggle — TBAA,
    // basic-aa, scev-aa, and CFL-steens/anders-style analyses are wired in
    // via registerFunctionAnalyses/registerModuleAnalyses (performed by
    // this class's setup regardless), and escape-style optimization comes
    // from the standard O2/O3 pipeline's SROA + always-run
    // AttributorPass/ArgumentPromotionPass, which mark pointer arguments
    // noalias/nocapture once they're provably non-escaping. This flag
    // gates whether PerformanceEngine explicitly re-schedules an extra
    // AttributorPass module pass after the main pipeline runs, to squeeze
    // out escape-analysis-driven noalias annotations the single default
    // pipeline pass may not have reached a fixpoint on.
    bool enableAliasEscapeAnalysis = true;
};

// Aggressively tunes and runs an LLVM optimization + codegen pipeline for
// `module`, using a TargetMachine built according to `options`. This is
// the single entry point most callers want: it builds the TargetMachine,
// runs the tuned pass pipeline, and hands back the TargetMachine (the
// caller needs it again for object-code emission via
// TargetMachine::addPassesToEmitFile, exactly as
// LLVMBackend::emitObjectCode() already does with its own, less-tuned
// TargetMachine).
class PerformanceEngine {
public:
    explicit PerformanceEngine(PerformanceOptions options = {});

    // Builds a host- (or explicitly-triple-) targeted llvm::TargetMachine
    // per options_.useNativeCpu / options_.targetTriple. Does not touch
    // `module`. Returns nullptr (and appends to diagnostics()) if LLVM
    // could not resolve a Target for the requested/detected triple.
    std::unique_ptr<llvm::TargetMachine> createHostTargetMachine();

    // Runs the fully-tuned PassBuilder pipeline (loop unrolling,
    // vectorization, aggressive inlining/devirtualization, alias/escape
    // analysis) over `module`, targeting `targetMachine`. `module`'s data
    // layout is set from `targetMachine` as a side effect (required for
    // the vectorizer and SLP passes to see accurate type-size / alignment
    // information). Returns true on success.
    bool optimize(llvm::Module& module, llvm::TargetMachine& targetMachine);

    // Convenience overload: builds the TargetMachine internally via
    // createHostTargetMachine() and then calls optimize() above. Returns
    // the TargetMachine so the caller can reuse it for emitObjectCode()-
    // style output (its lifetime is independent of this PerformanceEngine
    // instance). Returns nullptr on any failure (check diagnostics()).
    std::unique_ptr<llvm::TargetMachine> optimizeWithHostTuning(llvm::Module& module);

    const std::vector<std::string>& diagnostics() const { return diagnostics_; }

private:
    PerformanceOptions options_;
    std::vector<std::string> diagnostics_;

    void diag(const std::string& message);

    // Resolves options_.baseLevel vs. the vectorizer-floor / clamps as
    // documented on PerformanceOptions::minVectorizeLevel.
    llvm::OptimizationLevel effectiveLevel() const;
};

} // namespace nova::backend
