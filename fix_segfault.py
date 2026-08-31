import re
with open('nova_rt.cpp', 'r') as f:
    c = f.read()

# ख़राब main फंक्शन को हटाएं
c = re.sub(r'int main\(int argc, char\*\* argv\).*', '', c, flags=re.DOTALL)

# सही main फंक्शन को extern "C" के बाहर जोड़ें
safe_main = """
} // extern "C"

// LLVM द्वारा जनरेट किया गया असली Nova एंट्री पॉइंट (यह void रिटर्न करता है)
extern "C" void nova_fn___nova_main();

int main(int argc, char** argv) {
    // 1. Nova प्रोग्राम रन करें!
    nova_fn___nova_main();
    
    // 2. Concurrency/Thread Pool को सुरक्षित तरीके से बंद करें
    nova_rt_concurrency_shutdown();
    
    return 0;
}
"""
c += safe_main

with open('nova_rt.cpp', 'w') as f:
    f.write(c)

print("Segfault & ABI Fix Applied Successfully!")
