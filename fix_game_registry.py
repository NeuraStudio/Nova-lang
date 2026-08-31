with open('nova_rt.cpp', 'r') as f:
    c = f.read()

if 'nova_game_register' not in c:
    c = c.replace('extern "C" void nova_ecosystem_register();', 'extern "C" void nova_ecosystem_register();\nextern "C" void nova_game_register();')
    c = c.replace('nova_ecosystem_register();', 'nova_ecosystem_register();\n    nova_game_register();')
    with open('nova_rt.cpp', 'w') as f:
        f.write(c)
    print("Game Native Registry Patched Successfully!")
