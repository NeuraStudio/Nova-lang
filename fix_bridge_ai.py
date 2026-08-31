with open('nova_bridge.cpp', 'r') as f:
    c = f.read()

if 'nova_ai_register' not in c:
    c = c.replace('extern void nova_game_register();', 'extern void nova_game_register();\n    extern void nova_ai_register();')
    c = c.replace('nova_game_register();', 'nova_game_register();\n        nova_ai_register();')
    with open('nova_bridge.cpp', 'w') as f:
        f.write(c)
print("Nova Bridge updated for AI Engine!")
