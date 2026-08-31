// GCCBackend.cpp — Nova SSA IR -> standard C99/C++17 and GCC toolchain runner.
#include "GCCBackend.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace nova::backend {
namespace {

using nova::ir::BasicBlock;
using nova::ir::ConstantValue;
using nova::ir::Function;
using nova::ir::Instruction;
using nova::ir::Module;
using nova::ir::Opcode;
using nova::ir::Type;
using nova::ir::TypeKind;
using nova::ir::Value;
using nova::ir::ValueKind;
using nova::ir::ValuePtr;

std::string join(const std::vector<std::string>& xs, const std::string& sep) {
    std::ostringstream os;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (i) os << sep;
        os << xs[i];
    }
    return os.str();
}

bool hasSuffix(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

GCCBackend::GCCBackend(GCCBackendOptions options)
    : options_(std::move(options)) {}

void GCCBackend::diag(const std::string& message) {
    diagnostics_.push_back(message);
}

bool GCCBackend::lower(const Module& module) {
    diagnostics_.clear();
    source_.clear();
    module_ = &module;
    valueNames_.clear();
    blockLabels_.clear();
    functionNames_.clear();
    globalNames_.clear();
    phiEdges_.clear();

    prepareNames();
    preparePhiEdges();

    if (!buildSource()) {
        module_ = nullptr;
        return false;
    }
    return true;
}

bool GCCBackend::emitC(const std::string& filename) const {
    if (source_.empty()) {
        const_cast<GCCBackend*>(this)->diag(
            "GCCBackend::emitC called before successful lower()");
        return false;
    }

    std::ofstream out(filename, std::ios::binary);
    if (!out) {
        const_cast<GCCBackend*>(this)->diag(
            "cannot open generated C file: " + filename);
        return false;
    }
    out << source_;
    if (!out.good()) {
        const_cast<GCCBackend*>(this)->diag(
            "failed while writing generated C file: " + filename);
        return false;
    }
    return true;
}

void GCCBackend::prepareNames() {
    if (!module_) return;

    for (const auto& fnPtr : module_->functions) {
        functionNames_[fnPtr.get()] =
            "nova_fn_" + sanitizeIdentifier(fnPtr->name) + "_" + stableHash(fnPtr->name);

        for (const auto& bbPtr : fnPtr->blocks) {
            blockLabels_[bbPtr.get()] =
                "nova_bb_" + sanitizeIdentifier(bbPtr->name) + "_" +
                std::to_string(bbPtr->id);
        }

        for (const auto& arg : fnPtr->arguments) {
            if (arg) {
                valueNames_[arg.get()] =
                    "nova_v_" + std::to_string(arg->id ? arg->id : valueNames_.size() + 1);
            }
        }

        for (const auto& bbPtr : fnPtr->blocks) {
            for (const auto& inst : bbPtr->instructions) {
                if (!inst) continue;
                if (inst->opcode == Opcode::Nop ||
                    inst->opcode == Opcode::Jump ||
                    inst->opcode == Opcode::CondBranch ||
                    inst->opcode == Opcode::Return ||
                    inst->opcode == Opcode::Store ||
                    inst->opcode == Opcode::AsyncSuspend ||
                    inst->opcode == Opcode::AsyncResume ||
                    inst->opcode == Opcode::Yield) {
                    continue;
                }
                if (inst->type.kind == TypeKind::Void) continue;
                valueNames_[inst.get()] =
                    "nova_v_" + std::to_string(inst->id ? inst->id : valueNames_.size() + 1);
            }
        }
    }

    for (const auto& kv : module_->globals) {
        const std::string& name = kv.first;
        const ValuePtr& value = kv.second;
        if (value) {
            globalNames_[value.get()] =
                "nova_global_" + sanitizeIdentifier(name) + "_" + stableHash(name);
        }
    }
}

void GCCBackend::preparePhiEdges() {
    if (!module_) return;

    for (const auto& fnPtr : module_->functions) {
        for (const auto& bbPtr : fnPtr->blocks) {
            for (const auto& instPtr : bbPtr->instructions) {
                if (!instPtr || instPtr->opcode != Opcode::Phi) continue;
                for (std::size_t i = 0; i < instPtr->operands.size(); ++i) {
                    auto it = instPtr->attributes.find("pred" + std::to_string(i));
                    if (it == instPtr->attributes.end()) continue;
                    for (const auto& predPtr : fnPtr->blocks) {
                        if (predPtr->name == it->second) {
                            PhiEdge edge;
                            edge.phi = instPtr.get();
                            edge.predecessor = predPtr.get();
                            edge.temporary =
                                "nova_phi_tmp_" + std::to_string(instPtr->id) +
                                "_" + std::to_string(i);
                            phiEdges_.push_back(std::move(edge));
                            break;
                        }
                    }
                }
            }
        }
    }
}

bool GCCBackend::buildSource() {
    if (!module_) return false;

    std::string out;
    emitPreamble(out);
    emitGlobals(out);
    emitFunctionPrototypes(out);
    emitFunctions(out);
    if (options_.emitMainWrapper) emitMainWrapper(out);
    source_ = std::move(out);
    return true;
}

void GCCBackend::emitPreamble(std::string& out) const {
    out += "/* Generated by Nova GCCBackend. Do not edit. */\n";
    out += "/* Nova SSA is lowered to structured C labels/gotos with edge phi copies. */\n";
    if (options_.sourceMode == GCCSourceMode::C99) {
        out += "#include <stdint.h>\n";
        out += "#include <stddef.h>\n";
        out += "#include <stdbool.h>\n";
    } else {
        out += "#include <cstdint>\n";
        out += "#include <cstddef>\n";
        out += "#include <initializer_list>\n";
        out += "#include <vector>\n";
    }
    out += "\n";

    if (!options_.sourceHeader.empty()) {
        out += "/* User supplied bridge header. */\n";
        out += options_.sourceHeader;
        out += "\n\n";
    }

    if (options_.sourceMode == GCCSourceMode::Cxx17)
        out += "extern \"C\" {\n";

    out += "typedef struct NovaValue NovaValue;\n";
    out += "\n";
    out += "NovaValue* nova_rt_call(const char* name, NovaValue** args, int64_t argc);\n";
    out += "NovaValue* nova_rt_alloc(int64_t size);\n";
    out += "NovaValue* nova_rt_load(NovaValue* addr);\n";
    out += "void nova_rt_store(NovaValue* addr, NovaValue* value);\n";
    out += "NovaValue* nova_rt_const_int(int64_t v);\n";
    out += "NovaValue* nova_rt_const_float(double v);\n";
    out += "NovaValue* nova_rt_const_string(const char* v);\n";
    out += "NovaValue* nova_rt_const_bool(";
    out += options_.sourceMode == GCCSourceMode::Cxx17 ? "bool" : "bool";
    out += " v);\n";
    out += "NovaValue* nova_rt_const_null(void);\n";
    out += "int64_t nova_rt_to_int(NovaValue* v);\n";
    out += "double nova_rt_to_float(NovaValue* v);\n";
    out += "bool nova_rt_to_bool(NovaValue* v);\n";
    out += "NovaValue* nova_rt_from_int(int64_t v);\n";
    out += "NovaValue* nova_rt_from_float(double v);\n";
    out += "NovaValue* nova_rt_from_bool(bool v);\n";
    out += "NovaValue* nova_rt_add(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_sub(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_mul(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_div(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_mod(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_pow(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_eq(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_ne(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_lt(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_le(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_gt(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_ge(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_and(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_or(NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_neg(NovaValue*);\n";
    out += "NovaValue* nova_rt_not(NovaValue*);\n";
    out += "NovaValue* nova_rt_select(NovaValue*, NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_cast(NovaValue*, const char* typeName);\n";
    out += "NovaValue* nova_rt_index(NovaValue*, NovaValue* key);\n";
    out += "NovaValue* nova_rt_member(NovaValue*, const char* name);\n";
    out += "NovaValue* nova_rt_slice(NovaValue*, NovaValue*, NovaValue*, NovaValue*);\n";
    out += "NovaValue* nova_rt_make_array(NovaValue**, int64_t count);\n";
    out += "NovaValue* nova_rt_make_map(NovaValue**, int64_t pairCount);\n";
    out += "NovaValue* nova_rt_make_tuple(NovaValue**, int64_t count);\n";
    out += "bool nova_rt_error_check(NovaValue*);\n";
    out += "NovaValue* nova_rt_await(NovaValue*);\n";
    out += "NovaValue* nova_rt_yield(NovaValue*);\n";
    out += "void nova_rt_async_suspend(NovaValue*);\n";
    out += "void nova_rt_async_resume(NovaValue*);\n";

    if (options_.sourceMode == GCCSourceMode::Cxx17) {
        out += "} /* extern \"C\" */\n\n";
        out += "static NovaValue* nova_bridge_call(const char* name, "
               "std::initializer_list<NovaValue*> xs) {\n";
        out += "    std::vector<NovaValue*> a(xs);\n";
        out += "    return nova_rt_call(name, a.empty() ? nullptr : a.data(), "
               "static_cast<int64_t>(a.size()));\n";
        out += "}\n";
        out += "static NovaValue* nova_bridge_make_array(std::initializer_list<NovaValue*> xs) {\n";
        out += "    std::vector<NovaValue*> a(xs);\n";
        out += "    return nova_rt_make_array(a.empty() ? nullptr : a.data(), "
               "static_cast<int64_t>(a.size()));\n";
        out += "}\n";
        out += "static NovaValue* nova_bridge_make_map(std::initializer_list<NovaValue*> xs) {\n";
        out += "    std::vector<NovaValue*> a(xs);\n";
        out += "    return nova_rt_make_map(a.empty() ? nullptr : a.data(), "
               "static_cast<int64_t>(a.size()));\n";
        out += "}\n";
        out += "static NovaValue* nova_bridge_make_tuple(std::initializer_list<NovaValue*> xs) {\n";
        out += "    std::vector<NovaValue*> a(xs);\n";
        out += "    return nova_rt_make_tuple(a.empty() ? nullptr : a.data(), "
               "static_cast<int64_t>(a.size()));\n";
        out += "}\n\n";
    }
}

void GCCBackend::emitGlobals(std::string& out) const {
    if (!module_ || module_->globals.empty()) return;
    out += "/* Nova module globals. They are initialized to NULL here because "
           "the current IR has no module-init ordering contract. */\n";
    for (const auto& kv : module_->globals) {
        auto it = globalNames_.find(kv.second.get());
        if (it != globalNames_.end())
            out += "static NovaValue* " + it->second + " = NULL;\n";
    }
    out += "\n";
}

void GCCBackend::emitFunctionPrototypes(std::string& out) const {
    if (!module_) return;
    out += "/* Nova function declarations. Values use their ABI-preserving C type. */\n";
    for (const auto& fnPtr : module_->functions) {
        out += nativeType(fnPtr->returnType) + " " + functionName(*fnPtr) + "(";
        std::vector<std::string> params;
        for (const auto& arg : fnPtr->arguments) {
            params.push_back(nativeType(arg->type) + " " + valueName(*arg));
        }
        out += join(params, ", ");
        out += ");\n";
    }
    out += "\n";
}

void GCCBackend::emitFunctions(std::string& out) const {
    if (!module_) return;
    for (const auto& fnPtr : module_->functions) {
        out += emitFunction(*fnPtr);
        out += "\n";
    }
}

void GCCBackend::emitMainWrapper(std::string& out) const {
    if (!module_) return;

    const Function* entry = module_->findFunction("__nova_main");
    if (!entry) {
        entry = module_->findFunction("main");
    }

    out += "/* Native process entry point for GCC-produced executables. */\n";
    out += "int main(int argc, char** argv) {\n";
    out += "    (void)argc; (void)argv;\n";

    if (entry && entry->arguments.empty()) {
        const std::string call = functionName(*entry);
        if (entry->returnType.kind == TypeKind::Void) {
            out += "    " + call + "();\n";
            out += "    return 0;\n";
        } else if (entry->returnType.kind == TypeKind::Int) {
            out += "    return (int)" + call + "();\n";
        } else {
            out += "    (void)" + call + "();\n";
            out += "    return 0;\n";
        }
    } else {
        out += "    return 0;\n";
    }
    out += "}\n";
}

std::string GCCBackend::emitFunction(const Function& fn) const {
    std::ostringstream out;
    out << nativeType(fn.returnType) << " " << functionName(fn) << "(";

    std::vector<std::string> params;
    for (const auto& arg : fn.arguments)
        params.push_back(nativeType(arg->type) + " " + valueName(*arg));
    out << join(params, ", ") << ") {\n";

    if (fn.blocks.empty()) {
        if (fn.returnType.kind == TypeKind::Void)
            out << "    return;\n";
        else
            out << "    return " << coerceFromBoxed("nova_rt_const_null()", fn.returnType) << ";\n";
        out << "}\n";
        return out.str();
    }

    // Declare every SSA result at function scope. This is important for
    // C99 labels/gotos and also makes phi edge copies legal without jumping
    // over declarations.
    for (const auto& bbPtr : fn.blocks) {
        for (const auto& instPtr : bbPtr->instructions) {
            if (!instPtr) continue;
            if (instPtr->type.kind == TypeKind::Void) continue;
            auto it = valueNames_.find(instPtr.get());
            if (it == valueNames_.end()) continue;
            out << "    " << nativeType(instPtr->type) << " " << it->second << " = ";
            if (instPtr->opcode == Opcode::Phi)
                out << coerceFromBoxed("nova_rt_const_null()", instPtr->type);
            else
                out << coerceFromBoxed("nova_rt_const_null()", instPtr->type);
            out << ";\n";
        }
    }

    // Temporary storage for parallel phi copies on CFG edges.
    for (const auto& edge : phiEdges_) {
        bool belongs = false;
        for (const auto& bb : fn.blocks) {
            if (bb.get() == edge.predecessor) {
                belongs = true;
                break;
            }
        }
        if (!belongs) continue;
        out << "    " << nativeType(edge.phi->type) << " " << edge.temporary
            << " = " << coerceFromBoxed("nova_rt_const_null()", edge.phi->type) << ";\n";
    }

    out << "\n";

    for (const auto& bbPtr : fn.blocks) {
        const BasicBlock& bb = *bbPtr;
        out << blockLabel(&bb) << ":\n";
        for (const auto& instPtr : bb.instructions) {
            if (!instPtr) continue;
            if (instPtr->opcode == Opcode::Phi) continue;
            out << emitInstruction(fn, bb, *instPtr);
        }
        if (!bb.terminated()) {
            out << "    /* Recovered missing terminator. */\n";
            if (fn.returnType.kind == TypeKind::Void)
                out << "    return;\n";
            else
                out << "    return " << coerceFromBoxed("nova_rt_const_null()", fn.returnType) << ";\n";
        }
    }

    out << "}\n";
    return out.str();
}

std::string GCCBackend::emitInstruction(const Function& fn,
                                        const BasicBlock& bb,
                                        const Instruction& inst) const {
    std::ostringstream out;
    const std::string indent = "    ";

    switch (inst.opcode) {
        case Opcode::Nop:
            return indent + "/* nop */\n";

        case Opcode::Alloc: {
            std::string rhs = "nova_rt_alloc(8)";
            out << indent << valueName(inst) << " = "
                << coerceFromBoxed(rhs, inst.type) << ";\n";
            return out.str();
        }

        case Opcode::Load: {
            if (inst.operands.empty()) return indent + "/* invalid load */\n";
            out << indent << valueName(inst) << " = "
                << coerceFromBoxed("nova_rt_load(" + boxedExpr(inst.operands[0]) + ")",
                                   inst.type) << ";\n";
            return out.str();
        }

        case Opcode::Store:
            if (inst.operands.size() >= 2)
                out << indent << "nova_rt_store(" << boxedExpr(inst.operands[0])
                    << ", " << boxedExpr(inst.operands[1]) << ");\n";
            else
                out << indent << "/* invalid store */\n";
            return out.str();

        case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::Div:
        case Opcode::Mod: case Opcode::Pow:
        case Opcode::Eq: case Opcode::Ne: case Opcode::Lt: case Opcode::Le:
        case Opcode::Gt: case Opcode::Ge:
        case Opcode::And: case Opcode::Or: {
            if (inst.operands.size() != 2) {
                out << indent << "/* malformed binary instruction */\n";
                return out.str();
            }
            const Type& a = inst.operands[0]->type;
            const Type& b = inst.operands[1]->type;
            const bool nativeNumeric =
                (a.kind == TypeKind::Int || a.kind == TypeKind::Float) &&
                (b.kind == TypeKind::Int || b.kind == TypeKind::Float) &&
                inst.opcode != Opcode::Pow;
            const bool nativeBool =
                (a.kind == TypeKind::Bool && b.kind == TypeKind::Bool) &&
                (inst.opcode == Opcode::And || inst.opcode == Opcode::Or);

            std::string rhs;
            if (nativeNumeric || nativeBool) {
                std::string lhs = valueExpr(inst.operands[0]);
                std::string rhsOp = valueExpr(inst.operands[1]);
                if ((a.kind == TypeKind::Float) != (b.kind == TypeKind::Float)) {
                    if (a.kind == TypeKind::Float) rhsOp = "(double)(" + rhsOp + ")";
                    else lhs = "(double)(" + lhs + ")";
                }

                std::string op;
                switch (inst.opcode) {
                    case Opcode::Add: op = "+"; break;
                    case Opcode::Sub: op = "-"; break;
                    case Opcode::Mul: op = "*"; break;
                    case Opcode::Div: op = "/"; break;
                    case Opcode::Mod: op = "%"; break;
                    case Opcode::Eq: op = "=="; break;
                    case Opcode::Ne: op = "!="; break;
                    case Opcode::Lt: op = "<"; break;
                    case Opcode::Le: op = "<="; break;
                    case Opcode::Gt: op = ">"; break;
                    case Opcode::Ge: op = ">="; break;
                    case Opcode::And: op = "&&"; break;
                    case Opcode::Or: op = "||"; break;
                    default: break;
                }
                rhs = "(" + lhs + " " + op + " " + rhsOp + ")";
            } else {
                static const std::unordered_map<Opcode, std::string> rt = {
                    {Opcode::Add, "nova_rt_add"}, {Opcode::Sub, "nova_rt_sub"},
                    {Opcode::Mul, "nova_rt_mul"}, {Opcode::Div, "nova_rt_div"},
                    {Opcode::Mod, "nova_rt_mod"}, {Opcode::Pow, "nova_rt_pow"},
                    {Opcode::Eq, "nova_rt_eq"}, {Opcode::Ne, "nova_rt_ne"},
                    {Opcode::Lt, "nova_rt_lt"}, {Opcode::Le, "nova_rt_le"},
                    {Opcode::Gt, "nova_rt_gt"}, {Opcode::Ge, "nova_rt_ge"},
                    {Opcode::And, "nova_rt_and"}, {Opcode::Or, "nova_rt_or"}
                };
                rhs = rt.at(inst.opcode) + "(" + boxedExpr(inst.operands[0]) +
                      ", " + boxedExpr(inst.operands[1]) + ")";
            }
            if (nativeNumeric || nativeBool) {
                out << indent << valueName(inst) << " = " << rhs << ";\n";
            } else {
                out << indent << valueName(inst) << " = "
                    << coerceFromBoxed(rhs, inst.type) << ";\n";
            }
            return out.str();
        }

        case Opcode::Neg: {
            if (inst.operands.empty()) return indent + "/* malformed neg */\n";
            std::string rhs;
            if (inst.operands[0]->type.kind == TypeKind::Int ||
                inst.operands[0]->type.kind == TypeKind::Float) {
                rhs = "(-" + valueExpr(inst.operands[0]) + ")";
            } else {
                rhs = "nova_rt_neg(" + boxedExpr(inst.operands[0]) + ")";
            }
            if (inst.operands[0]->type.kind == TypeKind::Int ||
                inst.operands[0]->type.kind == TypeKind::Float) {
                out << indent << valueName(inst) << " = " << rhs << ";\n";
            } else {
                out << indent << valueName(inst) << " = "
                    << coerceFromBoxed(rhs, inst.type) << ";\n";
            }
            return out.str();
        }

        case Opcode::Not: {
            if (inst.operands.empty()) return indent + "/* malformed not */\n";
            std::string rhs;
            if (inst.operands[0]->type.kind == TypeKind::Bool)
                rhs = "(!" + valueExpr(inst.operands[0]) + ")";
            else
                rhs = "nova_rt_not(" + boxedExpr(inst.operands[0]) + ")";
            if (inst.operands[0]->type.kind == TypeKind::Bool) {
                out << indent << valueName(inst) << " = " << rhs << ";\n";
            } else {
                out << indent << valueName(inst) << " = "
                    << coerceFromBoxed(rhs, inst.type) << ";\n";
            }
            return out.str();
        }

        case Opcode::Select: {
            if (inst.operands.size() < 3) return indent + "/* malformed select */\n";
            std::string cond = isBool(inst.operands[0]->type)
                ? valueExpr(inst.operands[0])
                : "nova_rt_to_bool(" + boxedExpr(inst.operands[0]) + ")";
            auto branchExpr = [&](const ValuePtr& v) {
                if (v && v->type.kind == inst.type.kind && isNativeScalar(inst.type))
                    return valueExpr(v);
                if (isNativeScalar(inst.type))
                    return coerceFromBoxed(boxedExpr(v), inst.type);
                return valueExpr(v);
            };
            std::string rhs = "(" + cond + " ? " +
                              branchExpr(inst.operands[1]) + " : " +
                              branchExpr(inst.operands[2]) + ")";
            out << indent << valueName(inst) << " = " << rhs << ";\n";
            return out.str();
        }

        case Opcode::Cast: {
            if (inst.operands.empty()) return indent + "/* malformed cast */\n";
            std::string typeText = inst.attributes.count("type")
                ? inst.attributes.at("type") : inst.type.name;
            std::string rhs = "nova_rt_cast(" + boxedExpr(inst.operands[0]) +
                              ", " + literalString(typeText) + ")";
            out << indent << valueName(inst) << " = "
                << coerceFromBoxed(rhs, inst.type) << ";\n";
            return out.str();
        }

        case Opcode::Index: {
            if (inst.operands.size() < 2) return indent + "/* malformed index */\n";
            std::string rhs = "nova_rt_index(" + boxedExpr(inst.operands[0]) +
                              ", " + boxedExpr(inst.operands[1]) + ")";
            out << indent << valueName(inst) << " = "
                << coerceFromBoxed(rhs, inst.type) << ";\n";
            return out.str();
        }

        case Opcode::Member: {
            if (inst.operands.empty()) return indent + "/* malformed member */\n";
            std::string rhs = "nova_rt_member(" + boxedExpr(inst.operands[0]) +
                              ", " + literalString(memberName(inst)) + ")";
            out << indent << valueName(inst) << " = "
                << coerceFromBoxed(rhs, inst.type) << ";\n";
            return out.str();
        }

        case Opcode::Slice: {
            if (inst.operands.empty()) return indent + "/* malformed slice */\n";
            std::string start = inst.operands.size() > 1
                ? boxedExpr(inst.operands[1]) : "nova_rt_const_null()";
            std::string end = inst.operands.size() > 2
                ? boxedExpr(inst.operands[2]) : "nova_rt_const_null()";
            std::string step = inst.operands.size() > 3
                ? boxedExpr(inst.operands[3]) : "nova_rt_const_null()";
            std::string rhs = "nova_rt_slice(" + boxedExpr(inst.operands[0]) +
                              ", " + start + ", " + end + ", " + step + ")";
            out << indent << valueName(inst) << " = "
                << coerceFromBoxed(rhs, inst.type) << ";\n";
            return out.str();
        }

        case Opcode::MakeArray:
        case Opcode::MakeMap:
        case Opcode::MakeTuple: {
            std::string name = inst.opcode == Opcode::MakeArray ? "nova_rt_make_array" :
                               inst.opcode == Opcode::MakeMap ? "nova_rt_make_map" :
                               "nova_rt_make_tuple";
            std::vector<std::string> args;
            for (const auto& operand : inst.operands) args.push_back(boxedExpr(operand));
            std::string rhs = collectionExpression(name, args);
            out << indent << valueName(inst) << " = "
                << coerceFromBoxed(rhs, inst.type) << ";\n";
            return out.str();
        }

        case Opcode::Await: {
            if (inst.operands.empty()) return indent + "/* malformed await */\n";
            std::string rhs = "nova_rt_await(" + boxedExpr(inst.operands[0]) + ")";
            out << indent << valueName(inst) << " = "
                << coerceFromBoxed(rhs, inst.type) << ";\n";
            return out.str();
        }

        case Opcode::Yield:
            if (!inst.operands.empty())
                out << indent << "(void)nova_rt_yield(" << boxedExpr(inst.operands[0]) << ");\n";
            return out.str();

        case Opcode::AsyncSuspend:
            if (!inst.operands.empty())
                out << indent << "nova_rt_async_suspend(" << boxedExpr(inst.operands[0]) << ");\n";
            return out.str();

        case Opcode::AsyncResume:
            if (!inst.operands.empty())
                out << indent << "nova_rt_async_resume(" << boxedExpr(inst.operands[0]) << ");\n";
            return out.str();

        case Opcode::ErrorCheck: {
            if (inst.operands.empty()) return indent + "/* malformed error check */\n";
            std::string rhs = "nova_rt_error_check(" + boxedExpr(inst.operands[0]) + ")";
            out << indent << valueName(inst) << " = " << rhs << ";\n";
            return out.str();
        }

        case Opcode::Call:
        case Opcode::Invoke:
        case Opcode::Runtime: {
            if (inst.operands.empty()) {
                out << indent << "/* runtime instruction without callee */\n";
                return out.str();
            }

            std::string callee = inst.name;
            std::size_t firstArg = 0;
            if (auto* c = dynamic_cast<const ConstantValue*>(inst.operands[0].get());
                c && c->type.kind == TypeKind::String) {
                callee = c->literal;
                firstArg = 1;
            }

            std::vector<std::string> args;
            for (std::size_t i = firstArg; i < inst.operands.size(); ++i)
                args.push_back(boxedExpr(inst.operands[i]));

            std::string rhs = runtimeCallExpression(callee, args);
            if (inst.type.kind != TypeKind::Void) {
                out << indent << valueName(inst) << " = "
                    << coerceFromBoxed(rhs, inst.type) << ";\n";
            } else {
                out << indent << "(void)" << rhs << ";\n";
            }

            if (inst.opcode == Opcode::Invoke && inst.successors.size() >= 2) {
                // The Nova/LLVM ABI defines nova_rt_error_check() as true on
                // the successful path. Invoke successors are [normal,error].
                out << emitTerminator(fn, bb, inst);
            }
            return out.str();
        }

        case Opcode::Jump:
        case Opcode::CondBranch:
        case Opcode::Return:
            return emitTerminator(fn, bb, inst);

        case Opcode::Phi:
            return {};

        default:
            out << indent << "/* unsupported opcode " << static_cast<int>(inst.opcode)
                << " lowered conservatively */\n";
            if (inst.type.kind != TypeKind::Void)
                out << indent << valueName(inst) << " = "
                    << coerceFromBoxed("nova_rt_const_null()", inst.type) << ";\n";
            return out.str();
    }
}

std::string GCCBackend::emitTerminator(const Function& fn,
                                       const BasicBlock& bb,
                                       const Instruction& inst) const {
    std::ostringstream out;
    const std::string indent = "    ";

    auto emitEdge = [&](const BasicBlock* target) {
        if (!target) return std::string("    /* missing CFG target */\n");
        std::ostringstream e;

        // First evaluate all incoming phi expressions into edge-local
        // temporaries. This implements SSA parallel-copy semantics even when
        // phis form cycles.
        for (const auto& edge : phiEdges_) {
            if (edge.predecessor != &bb || edge.phi == nullptr) continue;
            const Instruction* phi = edge.phi;
            for (std::size_t i = 0; i < phi->operands.size(); ++i) {
                auto it = phi->attributes.find("pred" + std::to_string(i));
                if (it == phi->attributes.end() || it->second != bb.name) continue;

                bool targetContainsPhi = false;
                for (const auto& targetBB : fn.blocks) {
                    if (targetBB.get() != target) continue;
                    for (const auto& targetInst : targetBB->instructions) {
                        if (targetInst.get() == phi) {
                            targetContainsPhi = true;
                            break;
                        }
                    }
                }
                if (!targetContainsPhi) continue;

                const ValuePtr& incoming = phi->operands[i];
                std::string incomingExpr;
                if (incoming && incoming->type.kind == phi->type.kind &&
                    isNativeScalar(phi->type)) {
                    incomingExpr = valueExpr(incoming);
                } else if (isNativeScalar(phi->type)) {
                    incomingExpr = coerceFromBoxed(boxedExpr(incoming), phi->type);
                } else {
                    incomingExpr = valueExpr(incoming);
                }
                e << indent << edge.temporary << " = " << incomingExpr << ";\n";
                break;
            }
        }

        // Commit all phi copies after every incoming expression has been
        // evaluated.
        for (const auto& edge : phiEdges_) {
            if (edge.predecessor != &bb || edge.phi == nullptr) continue;
            const Instruction* phi = edge.phi;
            bool belongsToTarget = false;
            for (const auto& targetBB : fn.blocks) {
                if (targetBB.get() != target) continue;
                for (const auto& targetInst : targetBB->instructions)
                    if (targetInst.get() == phi) belongsToTarget = true;
            }
            if (belongsToTarget)
                e << indent << valueName(*phi) << " = " << edge.temporary << ";\n";
        }

        e << indent << "goto " << blockLabel(target) << ";\n";
        return e.str();
    };

    switch (inst.opcode) {
        case Opcode::Jump:
            if (inst.successors.empty())
                return indent + "return;\n";
            return emitEdge(inst.successors[0]);

        case Opcode::CondBranch: {
            if (inst.operands.empty() || inst.successors.size() < 2)
                return indent + "return;\n";
            std::string cond = isBool(inst.operands[0]->type)
                ? valueExpr(inst.operands[0])
                : "nova_rt_to_bool(" + boxedExpr(inst.operands[0]) + ")";

            // C's if statement preserves the IR branch order.
            std::ostringstream e;
            e << indent << "if (" << cond << ") {\n";
            e << emitEdge(inst.successors[0]);
            e << indent << "} else {\n";
            e << emitEdge(inst.successors[1]);
            e << indent << "}\n";
            return e.str();
        }

        case Opcode::Return: {
            if (fn.returnType.kind == TypeKind::Void)
                return indent + "return;\n";

            if (inst.operands.empty())
                return indent + "return " +
                       coerceFromBoxed("nova_rt_const_null()", fn.returnType) + ";\n";

            const ValuePtr& rv = inst.operands[0];
            std::string returnExpr;
            if (rv && rv->type.kind == fn.returnType.kind &&
                isNativeScalar(fn.returnType)) {
                returnExpr = valueExpr(rv);
            } else if (isNativeScalar(fn.returnType)) {
                returnExpr = coerceFromBoxed(boxedExpr(rv), fn.returnType);
            } else {
                returnExpr = valueExpr(rv);
            }
            return indent + "return " + returnExpr + ";\n";
        }

        case Opcode::Invoke: {
            if (inst.successors.size() < 2)
                return indent + "return;\n";
            std::string resultName;
            auto it = valueNames_.find(&inst);
            if (it != valueNames_.end()) resultName = it->second;
            std::string check;
            if (resultName.empty()) {
                check = "false";
            } else if (inst.type.kind == TypeKind::Int) {
                check = "nova_rt_error_check(nova_rt_from_int(" + resultName + "))";
            } else if (inst.type.kind == TypeKind::Float) {
                check = "nova_rt_error_check(nova_rt_from_float(" + resultName + "))";
            } else if (inst.type.kind == TypeKind::Bool) {
                check = "nova_rt_error_check(nova_rt_from_bool(" + resultName + "))";
            } else {
                check = "nova_rt_error_check(" + resultName + ")";
            }
            std::ostringstream e;
            e << indent << "if (" << check << ") {\n";
            e << emitEdge(inst.successors[0]);
            e << indent << "} else {\n";
            e << emitEdge(inst.successors[1]);
            e << indent << "}\n";
            return e.str();
        }

        default:
            return {};
    }
}

std::string GCCBackend::valueExpr(const ValuePtr& value) const {
    if (!value) return "nova_rt_const_null()";

    auto g = globalNames_.find(value.get());
    if (g != globalNames_.end()) return g->second;

    auto n = valueNames_.find(value.get());
    if (n != valueNames_.end()) return n->second;

    if (value->kind == ValueKind::Constant) {
        const auto* c = dynamic_cast<const ConstantValue*>(value.get());
        if (!c) return "nova_rt_const_null()";
        switch (c->type.kind) {
            case TypeKind::Int:
                return "((int64_t)(" + (c->literal.empty() ? "0" : c->literal) + "))";
            case TypeKind::Float:
                return "((double)(" + (c->literal.empty() ? "0.0" : c->literal) + "))";
            case TypeKind::Bool:
                return std::string("(") +
                       ((c->literal == "true" || c->literal == "1") ? "true" : "false") + ")";
            case TypeKind::String:
                return "nova_rt_const_string(" + literalString(c->literal) + ")";
            case TypeKind::Null:
                return "nova_rt_const_null()";
            case TypeKind::Any:
            default:
                return "nova_rt_const_string(" + literalString(c->literal) + ")";
        }
    }

    if (value->kind == ValueKind::Undef)
        return "nova_rt_const_null()";

    const_cast<GCCBackend*>(this)->diag("unmapped Nova IR value: " + value->ref());
    return "nova_rt_const_null()";
}

std::string GCCBackend::boxedExpr(const ValuePtr& value) const {
    if (!value) return "nova_rt_const_null()";

    const std::string expr = valueExpr(value);
    switch (value->type.kind) {
        case TypeKind::Int:
            return "nova_rt_from_int(" + expr + ")";
        case TypeKind::Float:
            return "nova_rt_from_float(" + expr + ")";
        case TypeKind::Bool:
            return "nova_rt_from_bool(" + expr + ")";
        default:
            return expr;
    }
}

std::string GCCBackend::coerceFromBoxed(const std::string& expression,
                                        const Type& target) const {
    switch (target.kind) {
        case TypeKind::Int:
            return "nova_rt_to_int(" + expression + ")";
        case TypeKind::Float:
            return "nova_rt_to_float(" + expression + ")";
        case TypeKind::Bool:
            return "nova_rt_to_bool(" + expression + ")";
        default:
            return expression;
    }
}

std::string GCCBackend::coerceToNative(const std::string& expression,
                                       const Type& target) const {
    // Expression is already in the native type when it originates from an
    // IR value of the same concrete type. For a boxed ABI return, unbox it.
    switch (target.kind) {
        case TypeKind::Int:
            return "nova_rt_to_int(" + expression + ")";
        case TypeKind::Float:
            return "nova_rt_to_float(" + expression + ")";
        case TypeKind::Bool:
            return "nova_rt_to_bool(" + expression + ")";
        default:
            return expression;
    }
}

std::string GCCBackend::nativeType(const Type& type) const {
    switch (type.kind) {
        case TypeKind::Void: return "void";
        case TypeKind::Bool: return "bool";
        case TypeKind::Int: return "int64_t";
        case TypeKind::Float: return "double";
        // There is no portable unboxed C string ABI in the existing Nova
        // runtime contract. Strings therefore remain boxed NovaValue*.
        case TypeKind::String:
        case TypeKind::Null:
        case TypeKind::Any:
        case TypeKind::Pointer:
        case TypeKind::Aggregate:
        case TypeKind::Function:
        default:
            return "NovaValue*";
    }
}

bool GCCBackend::isNativeScalar(const Type& type) const {
    return type.kind == TypeKind::Bool ||
           type.kind == TypeKind::Int ||
           type.kind == TypeKind::Float;
}

bool GCCBackend::isVoid(const Type& type) const {
    return type.kind == TypeKind::Void;
}

bool GCCBackend::isBool(const Type& type) const {
    return type.kind == TypeKind::Bool;
}

std::string GCCBackend::runtimeCallExpression(const std::string& callee,
                                              const std::vector<std::string>& args) const {
    if (options_.sourceMode == GCCSourceMode::Cxx17) {
        std::ostringstream os;
        os << "nova_bridge_call(" << literalString(callee) << ", {";
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) os << ", ";
            os << args[i];
        }
        os << "})";
        return os.str();
    }

    std::ostringstream os;
    os << "nova_rt_call(" << literalString(callee) << ", ";
    if (args.empty()) os << "NULL";
    else {
        os << "(NovaValue*[]){";
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) os << ", ";
            os << args[i];
        }
        os << "}";
    }
    os << ", " << args.size() << ")";
    return os.str();
}

std::string GCCBackend::collectionExpression(const std::string& runtimeName,
                                             const std::vector<std::string>& args) const {
    if (options_.sourceMode == GCCSourceMode::Cxx17) {
        std::ostringstream os;
        const char* helper =
            runtimeName == "nova_rt_make_array" ? "nova_bridge_make_array" :
            runtimeName == "nova_rt_make_map" ? "nova_bridge_make_map" :
            "nova_bridge_make_tuple";
        os << helper << "({";
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) os << ", ";
            os << args[i];
        }
        os << "})";
        return os.str();
    }

    std::ostringstream os;
    os << runtimeName << "(";
    if (args.empty()) os << "NULL";
    else {
        os << "(NovaValue*[]){";
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) os << ", ";
            os << args[i];
        }
        os << "}";
    }
    os << ", " << args.size() << ")";
    return os.str();
}

std::string GCCBackend::sanitizeIdentifier(const std::string& text) const {
    std::string out;
    out.reserve(text.size() + 1);
    for (char ch : text) {
        unsigned char u = static_cast<unsigned char>(ch);
        if (std::isalnum(u) || ch == '_') out.push_back(ch);
        else out.push_back('_');
    }
    if (out.empty()) out = "anon";
    if (std::isdigit(static_cast<unsigned char>(out[0])))
        out.insert(out.begin(), '_');

    static const char* reserved[] = {
        "auto","break","case","char","const","continue","default","do","double",
        "else","enum","extern","float","for","goto","if","inline","int","long",
        "register","restrict","return","short","signed","sizeof","static","struct",
        "switch","typedef","union","unsigned","void","volatile","while","_Bool",
        "class","namespace","template","typename","public","private","protected",
        "virtual","operator","new","delete","this","true","false"
    };
    for (const char* kw : reserved) {
        if (out == kw) {
            out.insert(0, "nova_");
            break;
        }
    }
    return out;
}

std::string GCCBackend::stableHash(const std::string& text) const {
    // FNV-1a 64-bit; deterministic across builds and platforms.
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : text) {
        h ^= static_cast<std::uint64_t>(c);
        h *= 1099511628211ULL;
    }
    std::ostringstream os;
    os << std::hex << std::nouppercase << h;
    return os.str();
}

std::string GCCBackend::literalString(const std::string& text) const {
    std::ostringstream os;
    os << "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '\\': os << "\\\\"; break;
            case '"': os << "\\\""; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            case '\0': os << "\\0"; break;
            default:
                if (c < 0x20 || c >= 0x7f) {
                    os << "\\x" << std::hex << std::setw(2)
                       << std::setfill('0') << static_cast<int>(c)
                       << std::dec << std::setfill(' ');
                } else {
                    os << static_cast<char>(c);
                }
        }
    }
    os << "\"";
    return os.str();
}

std::string GCCBackend::memberName(const Instruction& inst) const {
    auto it = inst.attributes.find("member");
    if (it != inst.attributes.end()) return it->second;
    const std::string& n = inst.name;
    if (n.rfind("addr.", 0) == 0) return n.substr(5);
    return n;
}

std::string GCCBackend::blockLabel(const BasicBlock* bb) const {
    if (!bb) return "nova_missing_bb";
    auto it = blockLabels_.find(bb);
    if (it != blockLabels_.end()) return it->second;
    return "nova_bb_missing_" + std::to_string(bb->id);
}

std::string GCCBackend::functionName(const Function& fn) const {
    auto it = functionNames_.find(&fn);
    if (it != functionNames_.end()) return it->second;
    return "nova_fn_" + sanitizeIdentifier(fn.name) + "_" + stableHash(fn.name);
}

std::string GCCBackend::valueName(const Value& value) const {
    auto it = valueNames_.find(&value);
    if (it != valueNames_.end()) return it->second;
    if (value.id)
        return "nova_v_" + std::to_string(value.id);
    return "nova_v_unknown";
}

std::string GCCBackend::phiTemporary(const Instruction& phi,
                                     const BasicBlock& predecessor) const {
    for (const auto& edge : phiEdges_) {
        if (edge.phi == &phi && edge.predecessor == &predecessor)
            return edge.temporary;
    }
    return "nova_phi_tmp_missing";
}

// ============================================================================
// GCCToolchain
// ============================================================================

GCCToolchain::GCCToolchain(GCCToolchainOptions options)
    : options_(std::move(options)) {}

void GCCToolchain::diag(const std::string& message) {
    diagnostics_.push_back(message);
}

std::string GCCToolchain::quote(const std::string& arg) const {
#ifdef _WIN32
    // Windows command line quoting compatible with CommandLineToArgvW-style
    // parsing for ordinary compiler arguments.
    if (arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string q = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            q.append(backslashes * 2 + 1, '\\');
            q.push_back('"');
            backslashes = 0;
        } else {
            q.append(backslashes, '\\');
            backslashes = 0;
            q.push_back(c);
        }
    }
    q.append(backslashes * 2, '\\');
    q.push_back('"');
    return q;
#else
    if (arg.empty()) return "''";
    std::string q = "'";
    for (char c : arg) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    q += "'";
    return q;
#endif
}

std::string GCCToolchain::extension(const std::string& path) const {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string GCCToolchain::temporaryObjectPath(const std::string& input,
                                              const std::string& suffix) const {
    std::filesystem::path p(input);
    p.replace_extension(suffix);
    return p.string();
}

GCCCommandResult GCCToolchain::run(const std::vector<std::string>& argv,
                                    const std::string& workingHint) const {
    GCCCommandResult result;
    std::ostringstream command;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) command << ' ';
        command << quote(argv[i]);
    }
    result.command = command.str();

    std::filesystem::path logPath;
    try {
        std::filesystem::path base = workingHint.empty()
            ? std::filesystem::temp_directory_path()
            : std::filesystem::path(workingHint).parent_path();
        if (base.empty()) base = std::filesystem::temp_directory_path();
        logPath = base / ("nova_gcc_" + std::to_string(
            static_cast<unsigned long long>(
                std::hash<std::string>{}(result.command))) + ".log");
    } catch (...) {
        logPath = "nova_gcc_command.log";
    }

    std::string shellCommand = result.command + " > " + quote(logPath.string()) + " 2>&1";
    if (options_.verbose)
        std::fprintf(stderr, "[Nova GCC] %s\n", result.command.c_str());

    result.exitCode = std::system(shellCommand.c_str());

    {
        std::ifstream in(logPath, std::ios::binary);
        if (in) {
            std::ostringstream contents;
            contents << in.rdbuf();
            result.output = contents.str();
        }
    }

    std::error_code ec;
    std::filesystem::remove(logPath, ec);
    return result;
}

bool GCCToolchain::compileC(const std::string& sourceFile,
                            const std::string& objectFile) {
    diagnostics_.clear();
    std::vector<std::string> cmd;
    cmd.push_back(options_.gcc);
    // GCC cross toolchains select the target through the compiler driver
    // itself (for example aarch64-linux-gnu-gcc). Do not pass Clang's
    // "-target" option to GCC.
    if (!options_.sysroot.empty()) {
        cmd.push_back("--sysroot=" + options_.sysroot);
    }
    cmd.push_back("-std=" + options_.cStandard);
    cmd.push_back("-c");
    for (const auto& d : options_.includeDirectories) {
        cmd.push_back("-I" + d);
    }
    for (const auto& f : options_.cFlags) cmd.push_back(f);
    cmd.push_back(sourceFile);
    cmd.push_back("-o");
    cmd.push_back(objectFile);

    GCCCommandResult r = run(cmd, objectFile);
    if (r.exitCode != 0) {
        diag("GCC C compilation failed: " + r.command);
        if (!r.output.empty()) diag(r.output);
        return false;
    }
    return true;
}

bool GCCToolchain::compileCxx(const std::string& sourceFile,
                              const std::string& objectFile) {
    diagnostics_.clear();
    std::vector<std::string> cmd;
    cmd.push_back(options_.gxx);
    // GCC cross toolchains select the target through the compiler driver
    // itself (for example aarch64-linux-gnu-gcc). Do not pass Clang's
    // "-target" option to GCC.
    if (!options_.sysroot.empty()) {
        cmd.push_back("--sysroot=" + options_.sysroot);
    }
    cmd.push_back("-std=" + options_.cxxStandard);
    cmd.push_back("-c");
    for (const auto& d : options_.includeDirectories) {
        cmd.push_back("-I" + d);
    }
    for (const auto& f : options_.cxxFlags) cmd.push_back(f);
    cmd.push_back(sourceFile);
    cmd.push_back("-o");
    cmd.push_back(objectFile);

    GCCCommandResult r = run(cmd, objectFile);
    if (r.exitCode != 0) {
        diag("G++ compilation failed: " + r.command);
        if (!r.output.empty()) diag(r.output);
        return false;
    }
    return true;
}

bool GCCToolchain::link(const std::vector<std::string>& objectFiles,
                        const std::string& outputExecutable,
                        const std::string& runtimeCpp) {
    if (objectFiles.empty() && runtimeCpp.empty()) {
        diag("link requested with no object files and no runtime source");
        return false;
    }

    std::vector<std::string> cmd;
    cmd.push_back(runtimeCpp.empty() ? options_.gxx : options_.gxx);

    if (!options_.targetTriple.empty()) {
        cmd.push_back("-target");
        cmd.push_back(options_.targetTriple);
    }
    if (!options_.sysroot.empty()) {
        cmd.push_back("--sysroot=" + options_.sysroot);
    }
    if (!runtimeCpp.empty()) {
        cmd.push_back("-std=" + options_.cxxStandard);
        for (const auto& f : options_.cxxFlags) cmd.push_back(f);
    }

    for (const auto& obj : objectFiles) cmd.push_back(obj);
    if (!runtimeCpp.empty()) cmd.push_back(runtimeCpp);

    for (const auto& d : options_.libraryDirectories)
        cmd.push_back("-L" + d);
    for (const auto& lib : options_.libraries)
        cmd.push_back("-l" + lib);
    for (const auto& f : options_.linkerFlags) cmd.push_back(f);

    cmd.push_back("-o");
    cmd.push_back(outputExecutable);

    GCCCommandResult r = run(cmd, outputExecutable);
    if (r.exitCode != 0) {
        diag("GCC linker failed: " + r.command);
        if (!r.output.empty()) diag(r.output);
        return false;
    }
    return true;
}

bool GCCToolchain::buildExecutable(const std::string& inputFile,
                                    const std::string& outputExecutable,
                                    const std::string& runtimeCpp) {
    diagnostics_.clear();

    if (!std::filesystem::exists(inputFile)) {
        diag("input file does not exist: " + inputFile);
        return false;
    }

    const std::string ext = extension(inputFile);
    const bool isObject = (ext == ".o" || ext == ".obj");

    std::vector<std::string> objects;
    std::string temporary;

    if (isObject) {
        objects.push_back(inputFile);
    } else if (ext == ".c") {
        temporary = temporaryObjectPath(inputFile, ".nova.gcc.o");
        if (!compileC(inputFile, temporary)) return false;
        objects.push_back(temporary);
    } else if (ext == ".cc" || ext == ".cpp" || ext == ".cxx") {
        temporary = temporaryObjectPath(inputFile, ".nova.gxx.o");
        if (!compileCxx(inputFile, temporary)) return false;
        objects.push_back(temporary);
    } else {
        diag("unsupported GCC input extension: " + ext);
        return false;
    }

    bool ok = link(objects, outputExecutable, runtimeCpp);

    if (!options_.keepTemporaryObjects && !temporary.empty()) {
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
    }
    return ok;
}

} // namespace nova::backend
