with open('nova_bridge.cpp', 'r') as f:
    c = f.read()

if 'nova_ffi_register' not in c:
    c = c.replace('extern void nova_ai_register();', 'extern void nova_ai_register();\n    extern void nova_ffi_register();')
    c = c.replace('nova_ai_register();', 'nova_ai_register();\n    nova_ffi_register();')
    with open('nova_bridge.cpp', 'w') as f:
        f.write(c)
print("Nova Bridge updated for Universal FFI!")
