with open('Semantic.hpp', 'r') as f:
    content = f.read()

# Symbol struct में constValue फील्ड्स जोड़ना
if 'bool hasConstValue = false;' not in content:
    content = content.replace(
        'std::string typeName;', 
        'std::string typeName;\n    bool hasConstValue = false;\n    std::string constValue;'
    )

# SymbolTable क्लास में setConstValue मेथड जोड़ना
if 'setConstValue(const std::string&' not in content:
    content = content.replace(
        'Symbol* findGlobalClass(const std::string& name);',
        'Symbol* findGlobalClass(const std::string& name);\n    bool setConstValue(const std::string& name, int declLine, int declCol, const std::string& value);'
    )

with open('Semantic.hpp', 'w') as f:
    f.write(content)

print("Semantic.hpp successfully patched!")
