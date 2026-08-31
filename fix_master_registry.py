import re
with open('nova_rt.cpp', 'r') as f:
    c = f.read()

# पुरानी खराब रजिस्ट्री को साफ करें
c = re.sub(r'#include <unordered_map>.*?extern "C" void nova_ecosystem_register\(\);\n?', '', c, flags=re.DOTALL)
c = re.sub(r'auto it = g_native_registry\.find\(fn\);.*?\}\n', '', c, flags=re.DOTALL)
c = re.sub(r'nova_ecosystem_register\(\);\n\s*', '', c)
c = re.sub(r'nova_mobile_register\(\);\n\s*', '', c)
c = re.sub(r'nova_game_register\(\);\n\s*', '', c)

# एकदम सही टॉप-लेवल रजिस्ट्री लगाएं
registry_def = """
#include <unordered_map>
static std::unordered_map<std::string, void*> g_native_registry;

extern "C" void nova_rt_register_native(const char* name, void* fnPtr, int arity) {
    g_native_registry[std::string(name)] = fnPtr;
}
extern "C" void nova_ecosystem_register();
extern "C" void nova_mobile_register();
extern "C" void nova_game_register();
"""
c = c.replace('#include <sstream>', '#include <sstream>\n' + registry_def)

# nova_rt_call के अंदर डिस्पैचर लगाएं
dispatch_logic = """
    auto it = g_native_registry.find(fn);
    if (it != g_native_registry.end()) {
        auto func = reinterpret_cast<NovaValue*(*)(NovaValue**, int64_t)>(it->second);
        return func(args, argc);
    }
"""
c = c.replace('std::string fn(name);', 'std::string fn(name);\n' + dispatch_logic)

# main() में तीनों इकोसिस्टम (11-13, 14, 15) को चालू करें
c = c.replace('nova_fn___nova_main();', 'nova_ecosystem_register();\n    nova_mobile_register();\n    nova_game_register();\n    nova_fn___nova_main();')

with open('nova_rt.cpp', 'w') as f:
    f.write(c)
print("nova_rt.cpp Master Registry Fixed!")
