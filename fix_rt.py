with open('nova_rt.cpp', 'r') as f:
    c = f.read()

# __nova_main को LLVM के असली नाम nova_fn___nova_main से बदलें
c = c.replace('extern "C" NovaValue* __nova_main();', 'extern "C" NovaValue* nova_fn___nova_main();')
c = c.replace('NovaValue* result = __nova_main();', 'NovaValue* result = nova_fn___nova_main();')

# nova_main को स्टैंडर्ड C++ main में बदलें
c = c.replace('int nova_main(void)', 'int main(int argc, char** argv)')

with open('nova_rt.cpp', 'w') as f:
    f.write(c)

print("Runtime Entry Point Fixed!")
