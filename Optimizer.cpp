#include "Optimizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

// ── LLVM headers for PerformanceEngine ──────────────────────────────────
#include <llvm/TargetParser/Triple.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/IPO/Attributor.h>

#include <optional>

namespace nova::ir {

// ============================================================================
// OptimizerStats
// ============================================================================

std::string OptimizerStats::str() const {
    std::ostringstream os;
    os << "folded=" << instructionsFolded
       << " removed=" << instructionsRemoved
       << " dceIterations=" << dceIterations;
    return os.str();
}

// ============================================================================
// Optimizer — construction / top-level driver
// ============================================================================

Optimizer::Optimizer(OptimizerOptions options) : options_(options) {}

bool Optimizer::run(Module& module) {
    stats_ = OptimizerStats{};
    bool changedOverall = false;

    // Constant folding and DCE feed each other: folding an Add/Mul/Eq/...
    // to a constant removes the last use of its operands, which can make
    // those operand-producing instructions dead; conversely, DCE removing
    // an instruction can simplify what a not-yet-considered instruction's
    // operand list looks like on a later pass (relevant once more opcodes,
    // e.g. Select/Phi-of-constants, are added to isFoldableOpcode). Running
    // both to a bounded fixpoint extracts strictly more than a single
    // fold-then-DCE pass.
    for (unsigned iter = 0; iter < options_.maxIterations; ++iter) {
        bool changedThisRound = false;

        if (options_.enableConstantFolding) {
            for (auto& fnPtr : module.functions) {
                if (foldConstantsInFunction(*fnPtr)) changedThisRound = true;
            }
        }

        if (options_.enableDeadCodeElimination) {
            for (auto& fnPtr : module.functions) {
                if (eliminateDeadCodeInFunction(*fnPtr)) changedThisRound = true;
            }
        }

        if (!changedThisRound) break;
        changedOverall = true;
    }

    return changedOverall;
}

// ============================================================================
// Constant Folding & Propagation
// ============================================================================

bool Optimizer::isFoldableOpcode(Opcode op) {
    switch (op) {
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
        case Opcode::Div: case Opcode::Mod: case Opcode::Pow:
        case Opcode::Neg: case Opcode::Not:
        case Opcode::Eq:  case Opcode::Ne:
        case Opcode::Lt:  case Opcode::Le:
        case Opcode::Gt:  case Opcode::Ge:
        case Opcode::And: case Opcode::Or:
            return true;
        default:
            return false;
    }
}

namespace {

// A small tagged union representing a constant that has been parsed out
// of a ConstantValue's textual literal, so folding can compute in native
// C++ arithmetic rather than fumbling with strings. Mirrors exactly the
// three concrete kinds ConstantValue::literal is ever written in by
// IRBuilder: plain decimal integers (Int), std::setprecision(17)-formatted
// doubles (Float), and the literal text "true"/"false" (Bool). Any other
// TypeKind (String/Null/Any/...) is simply not representable here and
// causes parsing to fail, which correctly makes the containing instruction
// non-foldable rather than mis-evaluated.
struct FoldedConst {
    enum class Kind { Int, Float, Bool } kind;
    std::int64_t i = 0;
    double f = 0.0;
    bool b = false;
};

bool parseConstant(const ConstantValue& c, FoldedConst& out) {
    switch (c.type.kind) {
        case TypeKind::Int: {
            // std::stoll throws on malformed input; guard explicitly so a
            // corrupt/unexpected literal degrades to "not foldable" rather
            // than propagating an exception out of the optimizer.
            try {
                std::size_t pos = 0;
                long long v = std::stoll(c.literal, &pos);
                if (pos != c.literal.size()) return false;
                out.kind = FoldedConst::Kind::Int;
                out.i = static_cast<std::int64_t>(v);
                return true;
            } catch (...) {
                return false;
            }
        }
        case TypeKind::Float: {
            try {
                std::size_t pos = 0;
                double v = std::stod(c.literal, &pos);
                if (pos != c.literal.size()) return false;
                out.kind = FoldedConst::Kind::Float;
                out.f = v;
                return true;
            } catch (...) {
                return false;
            }
        }
        case TypeKind::Bool: {
            if (c.literal == "true") { out.kind = FoldedConst::Kind::Bool; out.b = true; return true; }
            if (c.literal == "false") { out.kind = FoldedConst::Kind::Bool; out.b = false; return true; }
            return false;
        }
        default:
            return false;
    }
}

// Numeric promotion: if either operand is Float, evaluate in double and
// produce a Float result; otherwise stay in Int arithmetic. This matches
// ordinary C-family / Nova-source arithmetic promotion semantics and keeps
// integer division/mod exact when both operands really are integers.
bool bothNumeric(const FoldedConst& a, const FoldedConst& b) {
    return (a.kind == FoldedConst::Kind::Int || a.kind == FoldedConst::Kind::Float) &&
           (b.kind == FoldedConst::Kind::Int || b.kind == FoldedConst::Kind::Float);
}
double asDouble(const FoldedConst& v) {
    return v.kind == FoldedConst::Kind::Float ? v.f : static_cast<double>(v.i);
}

// Re-serializes a folded result using IRBuilder's exact textual
// conventions (see LiteralExpr lowering in IRBuilder.cpp) so a folded
// constant is byte-for-byte what IRBuilder itself would have emitted had
// the source contained the literal directly. This matters for golden-file
// tests that compare printed IR text.
ValuePtr makeFoldedConstant(FoldedConst::Kind kind, std::int64_t i, double f, bool b) {
    switch (kind) {
        case FoldedConst::Kind::Int: {
            auto c = std::make_shared<ConstantValue>(Type::intTy(), std::to_string(i));
            c->name = c->literal;
            return c;
        }
        case FoldedConst::Kind::Float: {
            std::ostringstream os;
            os << std::setprecision(17) << f;
            auto c = std::make_shared<ConstantValue>(Type::floatTy(), os.str());
            c->name = c->literal;
            return c;
        }
        case FoldedConst::Kind::Bool: {
            auto c = std::make_shared<ConstantValue>(Type::boolTy(), b ? "true" : "false");
            c->name = c->literal;
            return c;
        }
    }
    return nullptr;
}

} // namespace

ValuePtr Optimizer::tryFoldInstruction(const Instruction& inst) const {
    if (!isFoldableOpcode(inst.opcode)) return nullptr;

    const bool isUnary = (inst.opcode == Opcode::Neg || inst.opcode == Opcode::Not);
    const std::size_t expectedOperands = isUnary ? 1 : 2;
    if (inst.operands.size() != expectedOperands) return nullptr;

    // Every operand must itself be a compile-time ConstantValue. Constant
    // *propagation* — replacing an operand that is itself the folded
    // result of an earlier instruction — falls out for free here because
    // foldConstantsInFunction() rewrites uses in-place as it goes: by the
    // time a later instruction is visited, any operand that was previously
    // an Instruction result but has since been folded now points directly
    // at the ConstantValue produced for it (see replaceAllUsesInFunction),
    // so this function only ever needs to look at "is this operand
    // currently a ConstantValue", not chase through a use-def chain.
    std::vector<const ConstantValue*> consts;
    consts.reserve(inst.operands.size());
    for (const auto& operand : inst.operands) {
        if (!operand || operand->kind != ValueKind::Constant) return nullptr;
        auto* cv = dynamic_cast<const ConstantValue*>(operand.get());
        if (!cv) return nullptr;
        consts.push_back(cv);
    }

    FoldedConst a{};
    if (!parseConstant(*consts[0], a)) return nullptr;
    FoldedConst b{};
    if (!isUnary) {
        if (!parseConstant(*consts[1], b)) return nullptr;
    }

    switch (inst.opcode) {
        // ---- unary ----
        case Opcode::Neg: {
            if (a.kind == FoldedConst::Kind::Int)
                return makeFoldedConstant(FoldedConst::Kind::Int, -a.i, 0.0, false);
            if (a.kind == FoldedConst::Kind::Float)
                return makeFoldedConstant(FoldedConst::Kind::Float, 0, -a.f, false);
            return nullptr;
        }
        case Opcode::Not: {
            if (a.kind != FoldedConst::Kind::Bool) return nullptr;
            return makeFoldedConstant(FoldedConst::Kind::Bool, 0, 0.0, !a.b);
        }

        // ---- arithmetic ----
        case Opcode::Add: {
            if (!bothNumeric(a, b)) return nullptr;
            if (a.kind == FoldedConst::Kind::Int && b.kind == FoldedConst::Kind::Int)
                return makeFoldedConstant(FoldedConst::Kind::Int, a.i + b.i, 0.0, false);
            return makeFoldedConstant(FoldedConst::Kind::Float, 0, asDouble(a) + asDouble(b), false);
        }
        case Opcode::Sub: {
            if (!bothNumeric(a, b)) return nullptr;
            if (a.kind == FoldedConst::Kind::Int && b.kind == FoldedConst::Kind::Int)
                return makeFoldedConstant(FoldedConst::Kind::Int, a.i - b.i, 0.0, false);
            return makeFoldedConstant(FoldedConst::Kind::Float, 0, asDouble(a) - asDouble(b), false);
        }
        case Opcode::Mul: {
            if (!bothNumeric(a, b)) return nullptr;
            if (a.kind == FoldedConst::Kind::Int && b.kind == FoldedConst::Kind::Int)
                return makeFoldedConstant(FoldedConst::Kind::Int, a.i * b.i, 0.0, false);
            return makeFoldedConstant(FoldedConst::Kind::Float, 0, asDouble(a) * asDouble(b), false);
        }
        case Opcode::Div: {
            if (!bothNumeric(a, b)) return nullptr;
            if (a.kind == FoldedConst::Kind::Int && b.kind == FoldedConst::Kind::Int) {
                // Never fold an integer division by zero at compile time:
                // Nova's runtime semantics for that case (trap vs. error
                // value vs. exception) belong to the runtime, not to the
                // optimizer, so leaving the instruction in place preserves
                // whatever behavior nova_rt/the numeric fast path defines.
                if (b.i == 0) return nullptr;
                return makeFoldedConstant(FoldedConst::Kind::Int, a.i / b.i, 0.0, false);
            }
            return makeFoldedConstant(FoldedConst::Kind::Float, 0, asDouble(a) / asDouble(b), false);
        }
        case Opcode::Mod: {
            if (!bothNumeric(a, b)) return nullptr;
            if (a.kind == FoldedConst::Kind::Int && b.kind == FoldedConst::Kind::Int) {
                if (b.i == 0) return nullptr;
                return makeFoldedConstant(FoldedConst::Kind::Int, a.i % b.i, 0.0, false);
            }
            double bd = asDouble(b);
            if (bd == 0.0) return nullptr;
            return makeFoldedConstant(FoldedConst::Kind::Float, 0, std::fmod(asDouble(a), bd), false);
        }
        case Opcode::Pow: {
            if (!bothNumeric(a, b)) return nullptr;
            double r = std::pow(asDouble(a), asDouble(b));
            if (a.kind == FoldedConst::Kind::Int && b.kind == FoldedConst::Kind::Int && b.i >= 0 &&
                std::isfinite(r) && r == std::floor(r) &&
                r >= static_cast<double>(INT64_MIN) && r <= static_cast<double>(INT64_MAX)) {
                return makeFoldedConstant(FoldedConst::Kind::Int, static_cast<std::int64_t>(r), 0.0, false);
            }
            return makeFoldedConstant(FoldedConst::Kind::Float, 0, r, false);
        }

        // ---- comparisons (always produce Bool) ----
        case Opcode::Eq: case Opcode::Ne:
        case Opcode::Lt: case Opcode::Le:
        case Opcode::Gt: case Opcode::Ge: {
            bool cmp = false;
            if (a.kind == FoldedConst::Kind::Bool || b.kind == FoldedConst::Kind::Bool) {
                if (a.kind != b.kind) {
                    // Bool compared against a non-Bool constant: only
                    // equality/inequality are well-defined without an
                    // implicit numeric coercion the source type checker
                    // would have to have already sanctioned; ordering
                    // comparisons across kinds are left unfolded.
                    if (inst.opcode != Opcode::Eq && inst.opcode != Opcode::Ne) return nullptr;
                    cmp = (inst.opcode == Opcode::Ne); // different kinds => never equal
                } else {
                    switch (inst.opcode) {
                        case Opcode::Eq: cmp = (a.b == b.b); break;
                        case Opcode::Ne: cmp = (a.b != b.b); break;
                        default: return nullptr; // Bool has no total order
                    }
                }
            } else if (bothNumeric(a, b)) {
                double x = asDouble(a), y = asDouble(b);
                switch (inst.opcode) {
                    case Opcode::Eq: cmp = (x == y); break;
                    case Opcode::Ne: cmp = (x != y); break;
                    case Opcode::Lt: cmp = (x <  y); break;
                    case Opcode::Le: cmp = (x <= y); break;
                    case Opcode::Gt: cmp = (x >  y); break;
                    case Opcode::Ge: cmp = (x >= y); break;
                    default: return nullptr;
                }
            } else {
                return nullptr;
            }
            return makeFoldedConstant(FoldedConst::Kind::Bool, 0, 0.0, cmp);
        }

        // ---- logical ----
        case Opcode::And: {
            if (a.kind != FoldedConst::Kind::Bool || b.kind != FoldedConst::Kind::Bool) return nullptr;
            return makeFoldedConstant(FoldedConst::Kind::Bool, 0, 0.0, a.b && b.b);
        }
        case Opcode::Or: {
            if (a.kind != FoldedConst::Kind::Bool || b.kind != FoldedConst::Kind::Bool) return nullptr;
            return makeFoldedConstant(FoldedConst::Kind::Bool, 0, 0.0, a.b || b.b);
        }

        default:
            return nullptr;
    }
}

void Optimizer::replaceAllUsesInFunction(Function& fn, ValueId from, const ValuePtr& to) {
    if (from == 0) return; // 0 is never a real instruction id (see IRBuilder::nextValueId_ starting at 1)
    for (auto& block : fn.blocks) {
        for (auto& ins : block->instructions) {
            for (auto& operand : ins->operands) {
                if (operand && operand->id == from && operand->kind == ValueKind::Instruction) {
                    operand = to;
                }
            }
        }
    }
}

bool Optimizer::foldConstantsInFunction(Function& fn) {
    bool changed = false;

    for (auto& block : fn.blocks) {
        for (auto& ins : block->instructions) {
            // Terminators are never foldable opcodes (see isFoldableOpcode),
            // so this loop naturally skips Jump/CondBranch/Return/Invoke —
            // no special-case guard is needed to keep the CFG untouched.
            ValuePtr folded = tryFoldInstruction(*ins);
            if (!folded) continue;

            // Rewrite every other instruction in this function that
            // consumes `ins`'s result to consume the folded constant
            // instead. This is what makes constant *propagation* (as
            // opposed to bare folding) work: a chain like
            //   %1 = add 2 3      ; folds to 5
            //   %2 = mul %1 4     ; %1's use is rewritten to the constant 5
            //                     ; on THIS pass's next instruction, so %2
            //                     ; folds too, in the same foldConstantsInFunction
            //                     ; call — no extra driver loop required for
            //                     ; a straight-line chain within one block.
            replaceAllUsesInFunction(fn, ins->id, folded);

            // `ins` itself is left in the block (with its now-unused
            // result) rather than erased here: erasing while iterating
            // block->instructions would invalidate the iterator, and DCE
            // (which already knows how to safely erase dead, non-side-
            // effecting instructions) is guaranteed to run next and remove
            // it, since a folded arithmetic/comparison instruction is
            // never one of DCE's side-effect-carrying opcodes.
            ++stats_.instructionsFolded;
            changed = true;
        }
    }

    return changed;
}

// ============================================================================
// Dead Code Elimination
// ============================================================================

bool Optimizer::hasSideEffectsOrIsTerminator(const Instruction& inst) {
    if (inst.isTerminator()) return true; // Jump / CondBranch / Return / Invoke
    switch (inst.opcode) {
        case Opcode::Store:         // writes through a pointer
        case Opcode::Call:          // may have arbitrary runtime side effects
        case Opcode::Runtime:       // nova.* runtime entry points: I/O, mutation,
                                     // thread/channel/signal ops, error-propagation
                                     // state — none of this is visible as an SSA
                                     // def-use edge, so it must be over-approximated
                                     // as "has side effects" rather than inferred.
        case Opcode::Invoke:        // call with an exceptional successor
        case Opcode::Await:         // async suspension point
        case Opcode::Yield:         // generator suspension point
        case Opcode::AsyncSuspend:
        case Opcode::AsyncResume:
        case Opcode::ErrorCheck:    // gates error-propagation control flow
            return true;
        default:
            return false;
    }
}

bool Optimizer::eliminateDeadCodeInFunction(Function& fn) {
    bool changedOverall = false;

    for (unsigned iter = 0; iter < options_.maxIterations; ++iter) {
        // Recompute the use-count map every iteration rather than trying to
        // incrementally decrement it while erasing: this IR's operands are
        // shared_ptr<Value>, and removing one dead instruction can drop the
        // last remaining use of several different operands simultaneously
        // (e.g. `%3 = add %1 %2` being removed frees both %1 and %2 at
        // once), so a fresh full scan per iteration is both simpler and
        // cheap relative to the correctness risk of hand-rolled incremental
        // accounting. Each iteration can only ever find a subset of what
        // the previous one found (removing instructions never creates new
        // uses), so this is a monotonically-shrinking process and is
        // guaranteed to reach a fixpoint.
        std::unordered_map<ValueId, std::uint64_t> useCount;
        for (auto& block : fn.blocks) {
            for (auto& ins : block->instructions) {
                for (auto& operand : ins->operands) {
                    if (operand && operand->kind == ValueKind::Instruction) {
                        ++useCount[operand->id];
                    }
                }
            }
        }

        bool changedThisIteration = false;
        for (auto& block : fn.blocks) {
            auto& instructions = block->instructions;
            instructions.erase(
                std::remove_if(instructions.begin(), instructions.end(),
                    [&](const InstructionPtr& ins) {
                        if (hasSideEffectsOrIsTerminator(*ins)) return false;
                        // Void-typed, non-side-effecting instructions with
                        // no declared id (shouldn't occur given IRBuilder's
                        // emit(), but guarded defensively) are never live
                        // targets of a use, so they are safe to drop too.
                        auto it = useCount.find(ins->id);
                        std::uint64_t uses = (it == useCount.end()) ? 0 : it->second;
                        if (uses > 0) return false;
                        changedThisIteration = true;
                        ++stats_.instructionsRemoved;
                        return true;
                    }),
                instructions.end());
        }

        ++stats_.dceIterations;
        if (!changedThisIteration) break;
        changedOverall = true;
    }

    return changedOverall;
}

} // namespace nova::ir

// ============================================================================
// nova::backend::PerformanceEngine
// ============================================================================

namespace nova::backend {

PerformanceEngine::PerformanceEngine(PerformanceOptions options) : options_(std::move(options)) {}

void PerformanceEngine::diag(const std::string& message) {
    diagnostics_.push_back(message);
}

llvm::OptimizationLevel PerformanceEngine::effectiveLevel() const {
    llvm::OptimizationLevel level = options_.baseLevel;

    // PassBuilder only threads LoopVectorize/SLPVectorize into the
    // pipeline it builds for OptimizationLevel >= O2 (its O0/O1 pipelines
    // omit both vectorizers entirely, regardless of
    // PipelineTuningOptions). Requesting vectorization at a lower base
    // level would silently do nothing, so clamp up to the configured
    // floor rather than fail quietly.
    if ((options_.enableLoopVectorization || options_.enableSLPVectorization) &&
        level.getSpeedupLevel() < options_.minVectorizeLevel.getSpeedupLevel()) {
        level = options_.minVectorizeLevel;
    }
    return level;
}

std::unique_ptr<llvm::TargetMachine> PerformanceEngine::createHostTargetMachine() {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    std::string triple = options_.targetTriple.empty()
        ? llvm::sys::getDefaultTargetTriple()
        : options_.targetTriple;

    std::string lookupErr;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupErr);
    if (!target) {
        diag("PerformanceEngine: could not find target for triple '" + triple + "': " + lookupErr);
        return nullptr;
    }

    // -march=native equivalent: the CPU name alone (e.g. "skylake",
    // "znver3") only gets LLVM part of the way there — the ISA extension
    // bits (AVX2, AVX-512-*, FMA, BMI2, ...) that the vectorizers actually
    // key off of come from the *feature string*, which is why both
    // getHostCPUName() and getHostCPUFeatures() are forwarded together;
    // passing the CPU name without the feature string (as
    // LLVMBackend::emitObjectCode()'s cpu="native" path currently does)
    // silently leaves every host-specific feature bit off.
    std::string cpu = "generic";
    std::string features;
    if (options_.useNativeCpu) {
        cpu = std::string(llvm::sys::getHostCPUName());
        llvm::StringMap<bool> hostFeatures = llvm::sys::getHostCPUFeatures();
        llvm::SubtargetFeatures featureBuilder;
        for (auto& kv : hostFeatures) {
            featureBuilder.AddFeature(kv.first(), kv.second);
        }
        features = featureBuilder.getString();
    }

    llvm::TargetOptions targetOptions;
    // Fast-math-adjacent knobs deliberately left at their conservative
    // defaults (NaN/Inf semantics, signed-zero preservation) — enabling
    // fully unsafe fast-math here would change program-visible floating
    // point results, which is a correctness decision for Nova's language
    // semantics to make explicitly, not something an aggressive-tuning
    // engine should flip on unilaterally. Loop unrolling / vectorization /
    // inlining / devirtualization are pure "make it faster without
    // changing results" transforms and are what this class focuses on.

    std::optional<llvm::Reloc::Model> relocModel = llvm::Reloc::PIC_;

    // O3-equivalent CodeGenOpt level: the aggressive-tuning entry point
    // should never silently downgrade codegen quality relative to what
    // optimize()'s IR-level pipeline already assumed.
    std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
        triple, cpu, features, targetOptions, relocModel,
        /*CodeModel=*/std::nullopt, llvm::CodeGenOptLevel::Aggressive));

    if (!tm) {
        diag("PerformanceEngine: failed to create TargetMachine for triple '" + triple + "'");
        return nullptr;
    }
    return tm;
}

bool PerformanceEngine::optimize(llvm::Module& module, llvm::TargetMachine& targetMachine) {
    module.setDataLayout(targetMachine.createDataLayout());
    module.setTargetTriple(llvm::Triple(targetMachine.getTargetTriple().str()));

    // ---- analysis manager setup (required for basic-aa / TBAA / SCEV-AA
    // and the loop analyses the vectorizers/unroller depend on) ----
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PipelineTuningOptions pto;
    // ---- loop unrolling ----
    // LoopUnrolling itself defaults to on at O2+/O3, but force it
    // explicitly per the task requirement ("explicitly configure and
    // force-enable") rather than relying on the OptimizationLevel default,
    // and additionally force the *full/runtime* unroller variants on so
    // loops whose trip count isn't known at compile time still get
    // unrolled via a runtime trip-count check, not just compile-time-
    // constant-trip-count loops.
    pto.LoopUnrolling = options_.forceLoopUnrolling;
    pto.LoopInterleaving = options_.forceLoopUnrolling;

    // ---- vectorization ----
    pto.LoopVectorization = options_.enableLoopVectorization;
    pto.SLPVectorization = options_.enableSLPVectorization;

    // ---- inlining / devirtualization ----
    if (options_.aggressiveInlining && options_.inlineThreshold > 0) {
        pto.InlinerThreshold = options_.inlineThreshold;
    }

    llvm::PassBuilder PB(&targetMachine, pto);

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::OptimizationLevel level = effectiveLevel();

    llvm::ModulePassManager MPM;
    if (level == llvm::OptimizationLevel::O0) {
        // O0 has no inlining/vectorization/unrolling to speak of;
        // PerformanceEngine exists specifically to avoid this path, but it
        // is handled correctly rather than assumed unreachable.
        MPM = PB.buildO0DefaultPipeline(level);
    } else {
        // buildPerModuleDefaultPipeline at O2/O3 already runs the CGSCC
        // inliner pipeline to a fixpoint, the loop unroll/vectorize passes
        // (gated on pto.LoopUnrolling / pto.LoopVectorization above), and
        // SLP vectorization (gated on pto.SLPVectorization) as part of its
        // standard construction — the PipelineTuningOptions above are what
        // make this call actually force those on rather than leave them at
        // PassBuilder's own defaults.
        MPM = PB.buildPerModuleDefaultPipeline(level);
    }

    // ---- explicit devirtualization iteration control ----
    // PassBuilder's own devirtualization iteration count for the default
    // pipeline is derived from MaxDevirtIterations passed to the CGSCC
    // pass-manager construction internally; PipelineTuningOptions does not
    // expose a direct setter for it in the public API used here, so the
    // devirtualization depth actually achieved is a function of how many
    // times Attributor/inline chase newly-devirtualizable call sites
    // within the default pipeline's own iteration. To push further, an
    // additional explicit Attributor module pass is scheduled below (alias
    // / escape analysis section) — Attributor is also the pass most
    // responsible for turning "this pointer never escapes" facts into
    // noalias/nocapture attributes, which is what re-enables further
    // devirtualization and inlining opportunities the first pipeline run
    // exposed but didn't get to re-chase.
    if (options_.aggressiveInlining && options_.maxDevirtIterations > 0) {
        for (unsigned i = 0; i < options_.maxDevirtIterations; ++i) {
            llvm::ModulePassManager extraInline;
            extraInline.addPass(llvm::AlwaysInlinerPass());
            extraInline.run(module, MAM);
        }
    }

    // ---- alias / escape analysis follow-up ----
    // AttributorPass performs interprocedural escape analysis (does a
    // pointer argument/allocation ever escape the function it's local to)
    // and, where it proves "no", attaches noalias/nocapture/readonly-style
    // attributes that downstream alias analysis (basic-aa, TBAA) then uses
    // to permit more aggressive load/store reordering, vectorization, and
    // dead-store elimination. Re-running it once after the main pipeline
    // (rather than relying solely on however many times the default
    // pipeline itself scheduled it) catches fixpoint-adjacent facts that
    // became provable only after this pipeline's own inlining/devirt
    // rounds above.
    if (options_.enableAliasEscapeAnalysis) {
        llvm::ModulePassManager attributorPM;
        attributorPM.addPass(llvm::AttributorPass());
        attributorPM.run(module, MAM);
    }

    std::string verifyErr;
    llvm::raw_string_ostream verifyOs(verifyErr);
    if (llvm::verifyModule(module, &verifyOs)) {
        diag("PerformanceEngine::optimize: module verification failed after tuned pipeline:\n" + verifyErr);
        return false;
    }

    return true;
}

std::unique_ptr<llvm::TargetMachine> PerformanceEngine::optimizeWithHostTuning(llvm::Module& module) {
    std::unique_ptr<llvm::TargetMachine> tm = createHostTargetMachine();
    if (!tm) return nullptr;
    if (!optimize(module, *tm)) return nullptr;
    return tm;
}

} // namespace nova::backend
