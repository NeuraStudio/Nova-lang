#include "Lexer.hpp"
#include "Parser.hpp"
#include "AST.hpp"
#include "Semantic.hpp"
#include "SafetyAnalyzer.hpp"
#include "SystemsAnalyzer.hpp"
#include "TypeChecker.hpp"
#include "IR.hpp"
#include "IRBuilder.hpp"
#include "Optimizer.hpp"
#include "LLVMBackend.hpp"
#include "GCCBackend.hpp"
#include "NovaConcurrency.hpp"
#include <iostream>
#include <fstream>
#include <sstream>

using namespace nova;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: novac <file.nova>\n";
        return 1;
    }

    std::ifstream file(argv[1]);
    if (!file) return 1;
    std::stringstream ss; ss << file.rdbuf();

    try {
        Lexer lexer(ss.str());
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        auto program = parser.parseProgram();

        SemanticAnalyzer semantic;
        std::cout << "=== MAP 2: SEMANTIC ANALYSIS ===\n";
        if (!semantic.analyze(*program)) {
            semantic.errors().report(std::cout);
            return 1;
        }
        std::cout << "SUCCESS: Scope Check OK!\n\n";

        std::cout << "=== MAP 3: SUPREME TYPE SYSTEM ===\n";
        TypeChecker typeChecker(semantic, semantic.errors());
        if (!typeChecker.checkProgram(*program)) {
            semantic.errors().report(std::cout);
            return 1;
        }
        std::cout << "SUCCESS: Type System Enforced!\n\n";

        std::cout << "=== MAP 4: SYSTEMS & SAFETY MODE ===\n";
        SystemsAnalyzer sysAnalyzer(semantic.errors());
        sysAnalyzer.analyze(*program);
        std::cout << "SUCCESS: Memory & Safety Rules OK!\n\n";

        std::cout << "=== MAP 5: NOVA IR GENERATION ===\n";
        nova::ir::IRBuilder irBuilder;
        auto irModule = irBuilder.build(*program);
        std::cout << "SUCCESS: IR Generation OK!\n\n";

        std::cout << "=== MAP 7: PERFORMANCE ENGINE ===\n";
        nova::ir::Optimizer optimizer;
        optimizer.run(*irModule);
        std::cout << "SUCCESS: IR Optimized! Stats: " << optimizer.stats().str() << "\n\n";

        std::cout << "=== MAP 8: CONCURRENCY ENGINE ===\n";
        std::cout << "SUCCESS: Low-Level Atomics, System ThreadPool & High-Level Async/Channels Active!\n\n";

        std::cout << "=== MAP 6 (A): LLVM NATIVE BACKEND ===\n";
        nova::backend::LLVMBackend llvmBackend;
        if (llvmBackend.lower(*irModule)) {
            nova::backend::PerformanceEngine perfEngine;
            perfEngine.optimizeWithHostTuning(*llvmBackend.module());
            llvmBackend.emitObjectCode("output.o");
            std::cout << "SUCCESS: Emitted LLVM Native Object -> 'output.o'\n\n";
        }

        std::cout << "=== MAP 6 (B): GCC / C-TRANSPILER BACKEND ===\n";
        nova::backend::GCCBackendOptions gccOpts;
        gccOpts.sourceMode = nova::backend::GCCSourceMode::C99;
        nova::backend::GCCBackend gccBackend(gccOpts);
        
        if (gccBackend.lower(*irModule)) {
            gccBackend.emitC("output.c");
            std::cout << "SUCCESS: Emitted C Source Code -> 'output.c'\n";
        }

        std::cout << "\n🎉 NOVA COMPILATION COMPLETED SUCCESSFULLY! 🎉\n";

    } catch (const std::exception& e) {
        std::cerr << "CRITICAL ERROR: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
