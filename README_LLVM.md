# LLVMBackend — Nova IR → LLVM → native object code
This backend compiles Nova IR into LLVM Module and emits native object files (.o).
It uses a Hybrid approach:
- Fast Path for known Int/Float/Bool (Native LLVM Instructions).
- Boxed Path (%NovaValue*) for dynamic `Any` types, routing through `nova_rt_*` ABI.

## GCC Backend / C-Transpiler
Nova also supports a powerful GCC Backend. Instead of directly emitting machine code like LLVM, this backend transpiles Nova IR into standard, highly readable C99/C++17 code (`output.c`). 
It includes a `GCCToolchain` wrapper that automatically invokes the system's GCC/Clang to link the C code with `nova_rt.cpp` and produce a native executable.
