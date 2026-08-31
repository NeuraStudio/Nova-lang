with open('nova_bridge.cpp', 'r') as f:
    c = f.read()
if 'nova_metal_register' not in c:
    c = c.replace('extern void nova_ffi_register();', 'extern void nova_ffi_register();\n    extern void nova_metal_register();')
    c = c.replace('nova_ffi_register();', 'nova_ffi_register();\n    nova_metal_register();')
    with open('nova_bridge.cpp', 'w') as f:
        f.write(c)
print("Nova Bridge updated for Bare-Metal/OS Kernel!")
