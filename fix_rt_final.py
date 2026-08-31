import re
with open('nova_rt.cpp', 'r') as f:
    c = f.read()

# खराब डिक्लेरेशन्स को साफ करें
c = c.replace('extern "C" void extern "C" void', 'extern "C" void')
c = re.sub(r'extern "C" void nova_ecosystem_register\(\);\s+nova_mobile_register\(\);\s+nova_game_register\(\);\s+nova_fn___nova_main\(\);', 'extern "C" void nova_fn___nova_main();', c)

# पुराना main फंक्शन हटाएं और नया, साफ main() लगाएं
decls = """
extern "C" void nova_ecosystem_register();
extern "C" void nova_mobile_register();
extern "C" void nova_game_register();
extern "C" void nova_fn___nova_main();
extern "C" void nova_rt_concurrency_shutdown();

int main(int argc, char** argv) {
    // Register all ecosystem modules
    nova_ecosystem_register();
    nova_mobile_register();
    nova_game_register();
    
    // Run Nova Program
    nova_fn___nova_main();
    
    // Shutdown Threadpool
    nova_rt_concurrency_shutdown();
    return 0;
}
"""
c = re.sub(r'int main\(.*?\}.*', decls, c, flags=re.DOTALL)

with open('nova_rt.cpp', 'w') as f:
    f.write(c)
print("nova_rt.cpp Fixed Perfectly!")
