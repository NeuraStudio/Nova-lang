import re
with open('nova_rt.cpp', 'r') as f:
    content = f.read()

# Async & Generator Hooks के सेक्शन को हटा दें क्योंकि वो NovaConcurrency में पहले से हैं
content = re.sub(r'// =+?\n// Async & Generator Hooks\n// =+?.*?int\s+nova_main', 'int nova_main', content, flags=re.DOTALL)

with open('nova_rt.cpp', 'w') as f:
    f.write(content)
print("Duplicate async hooks removed from nova_rt.cpp!")
