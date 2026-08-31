// GCCBackend.hpp — Nova SSA IR -> GCC-compatible C99/C++17 bridge.
// C++17. Namespace: nova::backend.
#pragma once

#include "IR.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nova::backend {

enum class GCCSourceMode {
    C99,
    Cxx17
};

struct GCCBackendOptions {
    GCCSourceMode sourceMode = GCCSourceMode::C99;
    bool emitComments = true;
    bool emitMainWrapper = true;
    std::string sourceHeader;
};

class GCCBackend {
public:
    explicit GCCBackend(GCCBackendOptions options = {});

    GCCBackend(const GCCBackend&) = delete;
    GCCBackend& operator=(const GCCBackend&) = delete;

    // Lowers a Nova IR module into an internal textual C representation.
    // Call emitC() after a successful lower().
    bool lower(const nova::ir::Module& module);

    // Writes the generated C99/C++17 source to filename.
    bool emitC(const std::string& filename) const;

    // Returns the generated source without writing a file.
    const std::string& source() const { return source_; }

    // Diagnostics are warnings/errors accumulated by lower()/emitC().
    const std::vector<std::string>& diagnostics() const { return diagnostics_; }

private:
    GCCBackendOptions options_;
    const nova::ir::Module* module_ = nullptr;
    std::string source_;
    std::vector<std::string> diagnostics_;

    std::unordered_map<const nova::ir::Value*, std::string> valueNames_;
    std::unordered_map<const nova::ir::BasicBlock*, std::string> blockLabels_;
    std::unordered_map<const nova::ir::Function*, std::string> functionNames_;
    std::unordered_map<const nova::ir::Value*, std::string> globalNames_;

    struct PhiEdge {
        const nova::ir::Instruction* phi = nullptr;
        const nova::ir::BasicBlock* predecessor = nullptr;
        std::string temporary;
    };
    std::vector<PhiEdge> phiEdges_;

    bool buildSource();
    void emitPreamble(std::string& out) const;
    void emitGlobals(std::string& out) const;
    void emitFunctionPrototypes(std::string& out) const;
    void emitFunctions(std::string& out) const;
    void emitMainWrapper(std::string& out) const;

    void prepareNames();
    void preparePhiEdges();

    std::string emitFunction(const nova::ir::Function& fn) const;
    std::string emitInstruction(const nova::ir::Function& fn,
                                const nova::ir::BasicBlock& bb,
                                const nova::ir::Instruction& inst) const;
    std::string emitTerminator(const nova::ir::Function& fn,
                               const nova::ir::BasicBlock& bb,
                               const nova::ir::Instruction& inst) const;

    std::string valueExpr(const nova::ir::ValuePtr& value) const;
    std::string boxedExpr(const nova::ir::ValuePtr& value) const;
    std::string coerceFromBoxed(const std::string& expression,
                                const nova::ir::Type& target) const;
    std::string coerceToNative(const std::string& expression,
                               const nova::ir::Type& target) const;
    std::string nativeType(const nova::ir::Type& type) const;
    bool isNativeScalar(const nova::ir::Type& type) const;
    bool isVoid(const nova::ir::Type& type) const;
    bool isBool(const nova::ir::Type& type) const;

    std::string runtimeCallExpression(const std::string& callee,
                                       const std::vector<std::string>& args) const;
    std::string collectionExpression(const std::string& runtimeName,
                                     const std::vector<std::string>& args) const;

    std::string sanitizeIdentifier(const std::string& text) const;
    std::string stableHash(const std::string& text) const;
    std::string literalString(const std::string& text) const;
    std::string memberName(const nova::ir::Instruction& inst) const;
    std::string blockLabel(const nova::ir::BasicBlock* bb) const;
    std::string functionName(const nova::ir::Function& fn) const;
    std::string valueName(const nova::ir::Value& value) const;
    std::string phiTemporary(const nova::ir::Instruction& phi,
                             const nova::ir::BasicBlock& predecessor) const;

    void diag(const std::string& message);
};

struct GCCToolchainOptions {
    std::string gcc = "gcc";
    std::string gxx = "g++";

    std::string cStandard = "c99";
    std::string cxxStandard = "c++17";

    std::string targetTriple;
    std::string sysroot;

    std::vector<std::string> includeDirectories;
    std::vector<std::string> libraryDirectories;
    std::vector<std::string> libraries;

    std::vector<std::string> cFlags;
    std::vector<std::string> cxxFlags;
    std::vector<std::string> linkerFlags;

    bool keepTemporaryObjects = false;
    bool verbose = false;
};

struct GCCCommandResult {
    int exitCode = -1;
    std::string output;
    std::string command;
};

class GCCToolchain {
public:
    explicit GCCToolchain(GCCToolchainOptions options = {});

    GCCToolchain(const GCCToolchain&) = delete;
    GCCToolchain& operator=(const GCCToolchain&) = delete;

    // Accepts either a .c/.cc/.cpp source file or an already-built .o/.obj.
    // If runtimeCpp is non-empty it is compiled/linked as the Nova runtime.
    bool buildExecutable(const std::string& inputFile,
                         const std::string& outputExecutable,
                         const std::string& runtimeCpp = {});

    bool compileC(const std::string& sourceFile,
                  const std::string& objectFile);
    bool compileCxx(const std::string& sourceFile,
                    const std::string& objectFile);

    bool link(const std::vector<std::string>& objectFiles,
              const std::string& outputExecutable,
              const std::string& runtimeCpp = {});

    const std::vector<std::string>& diagnostics() const { return diagnostics_; }

private:
    GCCToolchainOptions options_;
    std::vector<std::string> diagnostics_;

    GCCCommandResult run(const std::vector<std::string>& argv,
                         const std::string& workingHint) const;

    std::string quote(const std::string& arg) const;
    std::string extension(const std::string& path) const;
    std::string temporaryObjectPath(const std::string& input,
                                    const std::string& suffix) const;
    void diag(const std::string& message);
};

} // namespace nova::backend
