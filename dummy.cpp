#include "runtime/nova_rt.hpp"
#include <iostream>
#include <string>

extern "C" int nova_main();

// 1. स्क्रीन पर प्रिंट करने का नेटिव C++ फंक्शन
extern "C" NovaValue* native_show(NovaValue** args, int64_t argc) {
    if (argc > 0) {
        NovaValue* str_val = nova_rt_to_string(args[0]);
        std::cout << nova_rt_to_cstr(str_val) << std::endl;
        nova_rt_release(str_val);
    }
    return nova_rt_const_null();
}

// 2. यूजर से इनपुट लेने का नेटिव C++ फंक्शन
extern "C" NovaValue* native_ask(NovaValue** args, int64_t argc) {
    if (argc > 0) {
        NovaValue* str_val = nova_rt_to_string(args[0]);
        std::cout << nova_rt_to_cstr(str_val);
        nova_rt_release(str_val);
    }
    std::string input;
    std::getline(std::cin, input);
    return nova_rt_from_string(input.c_str());
}

int main() {
    // रनटाइम इंजन स्टार्ट करने से ठीक पहले इन फंक्शन्स को Nova डिक्शनरी में रजिस्टर करें
    nova_rt_register_native("Nova.show", (void*)native_show, 1);
    nova_rt_register_native("Nova.ask.user", (void*)native_ask, 1);

    // अब असली गेम रन करें
    return nova_main();
}
