with open('Optimizer.cpp', 'r') as f:
    c = f.read()

# Headers Fix
c = c.replace('<llvm/Support/Host.h>', '<llvm/TargetParser/Host.h>')
c = c.replace('<llvm/ADT/Triple.h>', '<llvm/TargetParser/Triple.h>')
if '<llvm/TargetParser/SubtargetFeature.h>' not in c:
    c = c.replace('#include <llvm/TargetParser/Host.h>', '#include <llvm/TargetParser/Host.h>\n#include <llvm/TargetParser/SubtargetFeature.h>\n#include <llvm/Transforms/IPO/AlwaysInliner.h>')

# Syntax Fixes for LLVM 21
c = c.replace('llvm::CodeGenOpt::Aggressive', 'llvm::CodeGenOptLevel::Aggressive')
c = c.replace('module.setTargetTriple(targetMachine.getTargetTriple().str());', 'module.setTargetTriple(llvm::Triple(targetMachine.getTargetTriple().str()));')

with open('Optimizer.cpp', 'w') as f:
    f.write(c)

print("Optimizer.cpp patched for LLVM 21!")
