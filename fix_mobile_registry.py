with open('nova_rt.cpp', 'r') as f:
    c = f.read()

# अगर पहले से रजिस्टर नहीं है, तो जोड़ दें
if 'nova_mobile_register' not in c:
    c = c.replace('extern "C" void nova_ecosystem_register();', 'extern "C" void nova_ecosystem_register();\nextern "C" void nova_mobile_register();')
    c = c.replace('nova_ecosystem_register();', 'nova_ecosystem_register();\n    nova_mobile_register();')
    with open('nova_rt.cpp', 'w') as f:
        f.write(c)
    print("Mobile Native Registry Patched Successfully!")
