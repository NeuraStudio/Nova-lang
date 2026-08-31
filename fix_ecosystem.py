with open('nova_ecosystem.cpp', 'r') as f:
    c = f.read()
# String को const char* में बदलने वाला हेल्पर
helper = """
} // end extern C
// Helper to allow passing std::string directly to C-ABI
inline NovaValue* nova_rt_from_string(const std::string& s) { return nova_rt_from_string(s.c_str()); }
"""
c = c.replace('}\n\n// ═══════════════════════════════ platform includes', helper + '\n\n// ═══════════════════════════════ platform includes')
with open('nova_ecosystem.cpp', 'w') as f:
    f.write(c)
print("nova_ecosystem.cpp String Mismatch Fixed!")
