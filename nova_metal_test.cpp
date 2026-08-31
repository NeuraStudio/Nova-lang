// nova_metal_test.cpp — actual compile+run test for nova_metal.cpp.
// Provides minimal, real (not mocked-away) nova_rt_* implementations so
// nova_metal.cpp's actual production code runs unmodified against them —
// only the NovaValue boxing itself is a simple tagged struct here, since
// the real nova_rt.cpp doesn't exist in this sandbox yet, but every
// nova_metal_* function called below is the genuine, unmodified article.
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct NovaValue {
    enum class Kind { Int, Bool, String } kind;
    int64_t i = 0;
    bool b = false;
    std::string s;
};

extern "C" {
    int64_t nova_rt_to_int(NovaValue* v) { return v ? v->i : 0; }
    int nova_rt_to_bool(NovaValue* v) { return v ? (v->b ? 1 : 0) : 0; }
    const char* nova_rt_to_cstr(NovaValue* v) { return v ? v->s.c_str() : nullptr; }
    NovaValue* nova_rt_from_int(int64_t v) { auto* n = new NovaValue(); n->kind = NovaValue::Kind::Int; n->i = v; return n; }
    NovaValue* nova_rt_from_bool(int v) { auto* n = new NovaValue(); n->kind = NovaValue::Kind::Bool; n->b = (v != 0); return n; }
    NovaValue* nova_rt_from_string(const char* v) { auto* n = new NovaValue(); n->kind = NovaValue::Kind::String; n->s = v ? v : ""; return n; }

    struct RegisteredFn { void* fnPtr; int arity; };
    static std::vector<std::pair<std::string, RegisteredFn>> g_registry;
    void nova_rt_register_native(const char* name, void* fnPtr, int arity) {
        g_registry.push_back({std::string(name), {fnPtr, arity}});
    }
}

// Pull in the real nova_metal.cpp implementation directly (simplest way to
// compile-test a self-contained backend file against local stub ABI
// functions without a separate build system in this sandbox).
#include "nova_metal.cpp"

static NovaValue* mkInt(int64_t v) { return nova_rt_from_int(v); }
static NovaValue* mkBool(bool v) { return nova_rt_from_bool(v); }
static NovaValue* mkStr(const char* v) { return nova_rt_from_string(v); }

int main() {
    // ---- MMIO peek/poke: in-bounds round trip ----
    {
        NovaValue* addr = mkInt(1024);
        NovaValue* value = mkInt(0xDEADBEEF);
        NovaValue* size4 = mkInt(4);
        NovaValue* pokeResult = nova_metal_poke(addr, value, size4);
        assert(nova_rt_to_bool(pokeResult) == 1);

        NovaValue* readBack = nova_metal_peek(addr, size4);
        assert(nova_rt_to_int(readBack) == 0xDEADBEEF);
        printf("MMIO round trip OK: wrote 0x%llX, read back 0x%llX\n",
               (unsigned long long)0xDEADBEEF, (unsigned long long)nova_rt_to_int(readBack));
    }

    // ---- MMIO bounds checking: out-of-bounds access must NOT crash ----
    {
        NovaValue* farAddr = mkInt(999999999); // far beyond the 16MB simulated RAM
        NovaValue* size4 = mkInt(4);
        NovaValue* result = nova_metal_peek(farAddr, size4);
        assert(nova_rt_to_int(result) == 0); // safe defined fallback, no crash
        printf("Out-of-bounds peek handled safely (returned 0, no crash)\n");

        NovaValue* negativeAddr = mkInt(-500);
        NovaValue* pokeResult = nova_metal_poke(negativeAddr, mkInt(123), size4);
        assert(nova_rt_to_bool(pokeResult) == 0);
        printf("Negative-address poke rejected safely\n");

        NovaValue* hugeSize = mkInt(999); // not one of the allowed 1/2/4/8 widths
        NovaValue* badSizeResult = nova_metal_peek(mkInt(0), hugeSize);
        assert(nova_rt_to_int(badSizeResult) == 0);
        printf("Invalid access-width peek rejected safely\n");

        // The exact overflow case the boundsCheck logic exists to catch:
        // address + size wrapping around if computed naively.
        NovaValue* nearMaxAddr = mkInt(INT64_MAX - 2);
        NovaValue* overflowResult = nova_metal_peek(nearMaxAddr, mkInt(8));
        assert(nova_rt_to_int(overflowResult) == 0);
        printf("Overflow-prone address+size rejected safely\n");
    }

    // ---- virtual CPU registers ----
    {
        nova_metal_set_reg(mkInt(0), mkInt(42));   // EAX = 42
        nova_metal_set_reg(mkInt(4), mkInt(1000)); // PC = 1000
        assert(nova_rt_to_int(nova_metal_get_reg(mkInt(0))) == 42);
        assert(nova_rt_to_int(nova_metal_get_reg(mkInt(4))) == 1000);

        NovaValue* badReg = nova_metal_get_reg(mkInt(99));
        assert(nova_rt_to_int(badReg) == 0); // unknown register id handled safely
        printf("Virtual CPU registers OK: EAX=%lld PC=%lld\n",
               (long long)nova_rt_to_int(nova_metal_get_reg(mkInt(0))),
               (long long)nova_rt_to_int(nova_metal_get_reg(mkInt(4))));
    }

    // ---- GPIO ----
    {
        NovaValue* modeResult = nova_metal_gpio_mode(mkInt(13), mkInt(2)); // pin 13, OUTPUT
        assert(nova_rt_to_bool(modeResult) == 1);
        NovaValue* writeResult = nova_metal_gpio_write(mkInt(13), mkBool(true));
        assert(nova_rt_to_bool(writeResult) == 1);
        NovaValue* readResult = nova_metal_gpio_read(mkInt(13));
        assert(nova_rt_to_bool(readResult) == 1);

        // Writing to a pin never configured as OUTPUT must fail safely.
        NovaValue* badWrite = nova_metal_gpio_write(mkInt(5), mkBool(true));
        assert(nova_rt_to_bool(badWrite) == 0);

        // Out-of-range pin must fail safely, not crash.
        NovaValue* oobPin = nova_metal_gpio_mode(mkInt(9999), mkInt(2));
        assert(nova_rt_to_bool(oobPin) == 0);
        printf("GPIO simulation OK\n");
    }

    // ---- UART ----
    {
        NovaValue* result = nova_metal_uart_tx(mkStr("Hello from Nova bare-metal!"));
        assert(nova_rt_to_bool(result) == 1);
    }

    // ---- interrupts: trigger with no ISR (default handler), then register + trigger ----
    {
        nova_metal_trigger_interrupt(mkInt(7)); // no ISR yet -> default handler path, must not crash

        NovaValue* regResult = nova_metal_register_isr(mkInt(7), mkStr("log"));
        assert(nova_rt_to_bool(regResult) == 1);
        nova_metal_trigger_interrupt(mkInt(7)); // now handled by the registered 'log' ISR

        NovaValue* haltReg = nova_metal_register_isr(mkInt(8), mkStr("halt"));
        assert(nova_rt_to_bool(haltReg) == 1);
        nova_metal_trigger_interrupt(mkInt(8));

        NovaValue* badHandler = nova_metal_register_isr(mkInt(9), mkStr("nonexistent"));
        assert(nova_rt_to_bool(badHandler) == 0);
        printf("Interrupt/ISR simulation OK\n");
    }

    // ---- registration count sanity check ----
    nova_metal_register();
    assert(g_registry.size() == 11);
    printf("nova_metal_register() registered %zu native functions\n", g_registry.size());

    printf("ALL NOVA_METAL TESTS PASSED\n");
    return 0;
}
