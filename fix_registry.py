import re
with open('nova_rt.cpp', 'r') as f:
    c = f.read()

# Registry map और registration function जोड़ें
registry_code = """
#include <unordered_map>
static std::unordered_map<std::string, void*> g_native_registry;

extern "C" void nova_rt_register_native(const char* name, void* fnPtr, int arity) {
    g_native_registry[name] = fnPtr;
}
extern "C" void nova_ecosystem_register();
"""
c = c.replace('extern "C" NovaValue* nova_fn___nova_main();', registry_code + '\nextern "C" NovaValue* nova_fn___nova_main();')

# nova_rt_call के अंदर डायनामिक डिस्पैच जोड़ें
dispatch_hook = """
    auto it = g_native_registry.find(fn);
    if (it != g_native_registry.end()) {
        auto func = reinterpret_cast<NovaValue*(*)(NovaValue**, int64_t)>(it->second);
        return func(args, argc);
    }
"""
c = c.replace('std::string fn(name);', 'std::string fn(name);\n' + dispatch_hook)

# main() में ecosystem रजिस्टर करें
c = c.replace('nova_fn___nova_main();', 'nova_ecosystem_register();\n    nova_fn___nova_main();')

with open('nova_rt.cpp', 'w') as f:
    f.write(c)

print("Runtime Native Registry Patched Successfully!")
