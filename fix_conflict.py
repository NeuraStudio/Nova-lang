with open('nova_rt.cpp', 'r') as f:
    c = f.read()

# पुरानी (Line 22 वाली) गलत डिक्लेरेशन को हटा दें
c = c.replace('extern "C" NovaValue* nova_fn___nova_main();', '// Fixed conflicting declaration')

with open('nova_rt.cpp', 'w') as f:
    f.write(c)

print("Conflict Fixed Successfully!")
