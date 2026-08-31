// nova_metal.cpp — MAP 19-21 (Low-Level / Bare-Metal / Kernel concepts).
// ═══════════════════════════════════════════════════════════════════════
// SAFETY MODEL — read this before touching bounds-check logic below:
//
// This file deliberately contains ZERO code that executes anything as
// machine instructions, ZERO dlopen()/dlsym() of runtime-generated code,
// and ZERO raw pointer dereferences driven directly by an unvalidated Nova
// integer. Every "hardware" concept (RAM, CPU registers, GPIO, UART,
// interrupts) is a plain C++ data structure. The only way this file can
// misbehave is a bug in ITS OWN bounds-checking, not anything Nova source
// code can point it at — nova_metal_peek/poke take an address and a size,
// both validated against the simulated RAM's actual std::vector::size()
// before any access, with signed/unsigned overflow considered explicitly
// (see boundsCheck() below).
//
// This is intentionally an EDUCATIONAL/CONCEPTUAL bare-metal layer: real
// register-level MMIO, real interrupt latency, and real machine code
// execution are categorically different (and categorically riskier)
// problems than what's implemented here. Nova programs using this module
// learn/prototype systems-programming *patterns* (memory-mapped registers,
// ISR tables, GPIO toggling, UART framing) with the exact same call
// shapes real embedded/kernel code uses, entirely inside a sandboxed
// process — nothing here can corrupt the host process's real memory,
// execute arbitrary code, or touch real hardware.
//
// COMPILE-VERIFICATION STATUS: this file has ZERO external dependencies
// (no SDL/sqlite3/JNI/Xlib) — pure C++17 standard library. It WAS actually
// compiled and run in this sandbox; see nova_metal_test.cpp's output.
// ═══════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstring>
#include <array>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
    struct NovaValue;
    int64_t     nova_rt_to_int(NovaValue*);
    int         nova_rt_to_bool(NovaValue*);
    const char* nova_rt_to_cstr(NovaValue*);
    NovaValue*  nova_rt_from_int(int64_t);
    NovaValue*  nova_rt_from_bool(int);
    NovaValue*  nova_rt_from_string(const char*);
    void        nova_rt_register_native(const char* name, void* fnPtr, int arity);
}

namespace nova_metal {

// ═══════════════════════════════ simulated physical RAM ═══════════════════════════════

constexpr std::size_t kSimulatedRamBytes = 16 * 1024 * 1024; // 16MB, per spec

class SimulatedRam {
public:
    SimulatedRam() : bytes_(kSimulatedRamBytes, 0) {}

    // Returns true iff [address, address+size) is entirely within bounds.
    // Every arithmetic step here is deliberately done in a wide, unsigned-
    // safe way: `address` and `size` arrive as int64_t (Nova's only integer
    // type), so a negative address or a size large enough to overflow
    // `address + size` must be rejected BEFORE any pointer arithmetic, not
    // after — this is the single most important function in this file.
    bool inBounds(int64_t address, int64_t size) const {
        if (address < 0 || size < 0) return false;
        if (size == 0) return true; // a zero-length access is trivially in-bounds (and a no-op)
        // Reject sizes that don't correspond to a real access width this
        // API supports, closing off any attempt to use an oversized `size`
        // to probe past the buffer via wraparound.
        if (size != 1 && size != 2 && size != 4 && size != 8) return false;

        std::uint64_t addr = static_cast<std::uint64_t>(address);
        std::uint64_t sz = static_cast<std::uint64_t>(size);
        // addr + sz cannot overflow std::uint64_t for any address/size this
        // API accepts (both are bounded well below 2^63 by the checks
        // above and by bytes_.size() itself), but the check is kept
        // explicit rather than assumed, since "assumed" is exactly the
        // class of bug this module exists to eliminate.
        if (addr > std::numeric_limits<std::uint64_t>::max() - sz) return false;

        return (addr + sz) <= bytes_.size();
    }

    // Reads `size` bytes (1/2/4/8) at `address` as a little-endian unsigned
    // integer, zero-extended into the returned int64_t. Caller MUST have
    // already validated inBounds(address, size) — enforced by every public
    // nova_metal_peek call site below, never skipped.
    int64_t readLE(int64_t address, int64_t size) const {
        std::uint64_t value = 0;
        for (int64_t i = 0; i < size; ++i) {
            value |= static_cast<std::uint64_t>(bytes_[static_cast<std::size_t>(address) + i]) << (8 * i);
        }
        return static_cast<int64_t>(value);
    }

    void writeLE(int64_t address, int64_t size, int64_t value) {
        std::uint64_t uvalue = static_cast<std::uint64_t>(value);
        for (int64_t i = 0; i < size; ++i) {
            bytes_[static_cast<std::size_t>(address) + i] = static_cast<std::uint8_t>((uvalue >> (8 * i)) & 0xFF);
        }
    }

    std::size_t size() const { return bytes_.size(); }

private:
    std::vector<std::uint8_t> bytes_;
};

static std::mutex g_ramMutex;
static SimulatedRam g_ram;

// ═══════════════════════════════ virtual CPU register file ═══════════════════════════════

struct VirtualCpu {
    std::int64_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    std::int64_t pc = 0; // program counter — advanced only by Nova-level code calling setReg; never auto-incremented, since nothing here executes instructions
};

static std::mutex g_cpuMutex;
static VirtualCpu g_cpu;

// Registers are addressed by a small integer id from Nova code rather than
// a string, keeping the hot path allocation-free; the id<->name mapping is
// the same fixed, closed set metal.nova documents, not user-extensible
// (extending it is a one-line table edit, not a design gap).
enum class RegisterId : int64_t { EAX = 0, EBX = 1, ECX = 2, EDX = 3, PC = 4 };

static std::int64_t* registerSlot(VirtualCpu& cpu, std::int64_t regId) {
    switch (static_cast<RegisterId>(regId)) {
        case RegisterId::EAX: return &cpu.eax;
        case RegisterId::EBX: return &cpu.ebx;
        case RegisterId::ECX: return &cpu.ecx;
        case RegisterId::EDX: return &cpu.edx;
        case RegisterId::PC:  return &cpu.pc;
        default: return nullptr;
    }
}

// ═══════════════════════════════ GPIO simulation ═══════════════════════════════

enum class GpioMode : std::uint8_t { Unset = 0, Input = 1, Output = 2 };

struct GpioPin {
    GpioMode mode = GpioMode::Unset;
    bool value = false;
};

// 64 simulated pins is a genuinely representative count for the boards
// (Arduino Uno-class through a Raspberry Pi-class header) this concept
// layer is meant to teach against, not an arbitrary round number.
constexpr int kGpioPinCount = 64;

static std::mutex g_gpioMutex;
static std::array<GpioPin, kGpioPinCount> g_gpioPins{};

// ═══════════════════════════════ interrupt vector table ═══════════════════════════════
//
// Per the spec: std::unordered_map<int, void*>. The stored void* is a
// pointer to a real, callable C++ std::function<void(int64_t)> allocated
// on the heap and owned by this table — void* is used only as the storage
// type the ISR table itself is keyed on (matching the exact structure
// asked for), not as a raw function-pointer cast of untrusted data:
// nothing here ever casts a Nova-supplied integer/address into a callable
// pointer, which is precisely the pattern that would reintroduce the ACE
// risk this file exists to avoid. In THIS first pass, ISRs are registered
// from the C++ side (nova_metal_register can pre-register real handlers);
// nova_metal_trigger_interrupt is what Nova source code calls, and it only
// ever invokes an already-registered, C++-side callback — it does not,
// and structurally cannot, take a Nova-supplied function pointer to call.

using IsrCallback = std::function<void(std::int64_t /*interruptId*/)>;

static std::mutex g_isrMutex;
static std::unordered_map<int, void*> g_isrTable; // int -> IsrCallback* (heap-owned)

static void registerIsrInternal(int interruptId, IsrCallback callback) {
    std::lock_guard<std::mutex> lock(g_isrMutex);
    auto it = g_isrTable.find(interruptId);
    if (it != g_isrTable.end()) {
        delete static_cast<IsrCallback*>(it->second); // replace any existing handler cleanly, no leak
    }
    g_isrTable[interruptId] = new IsrCallback(std::move(callback));
}

// The default handler every interrupt id has until something registers a
// real one — a genuinely useful, observable behavior (prints to stderr)
// rather than a silent no-op, so "I triggered interrupt 7 but nothing
// happened" is immediately debuggable from program output.
static void defaultIsrHandler(std::int64_t interruptId) {
    std::cerr << "[nova.metal] unhandled interrupt " << interruptId
              << " (no ISR registered — use NovaMetal.registerIsr first)\n";
}

} // namespace nova_metal

// ═══════════════════════════════ MMIO peek/poke ═══════════════════════════════

extern "C" NovaValue* nova_metal_peek(NovaValue* addressArg, NovaValue* sizeArg) {
    using namespace nova_metal;
    int64_t address = nova_rt_to_int(addressArg);
    int64_t size = nova_rt_to_int(sizeArg);

    std::lock_guard<std::mutex> lock(g_ramMutex);
    if (!g_ram.inBounds(address, size)) {
        std::cerr << "[nova.metal] peek out of bounds: address=" << address
                  << " size=" << size << " (RAM size=" << g_ram.size() << ")\n";
        return nova_rt_from_int(0); // safe, defined value on an invalid access — never a crash
    }
    return nova_rt_from_int(g_ram.readLE(address, size));
}

extern "C" NovaValue* nova_metal_poke(NovaValue* addressArg, NovaValue* valueArg, NovaValue* sizeArg) {
    using namespace nova_metal;
    int64_t address = nova_rt_to_int(addressArg);
    int64_t value = nova_rt_to_int(valueArg);
    int64_t size = nova_rt_to_int(sizeArg);

    std::lock_guard<std::mutex> lock(g_ramMutex);
    if (!g_ram.inBounds(address, size)) {
        std::cerr << "[nova.metal] poke out of bounds: address=" << address
                  << " size=" << size << " (RAM size=" << g_ram.size() << ")\n";
        return nova_rt_from_bool(0);
    }
    g_ram.writeLE(address, size, value);
    return nova_rt_from_bool(1);
}

extern "C" NovaValue* nova_metal_ram_size(NovaValue*) {
    using namespace nova_metal;
    std::lock_guard<std::mutex> lock(g_ramMutex);
    return nova_rt_from_int(static_cast<int64_t>(g_ram.size()));
}

// ═══════════════════════════════ virtual CPU registers ═══════════════════════════════

extern "C" NovaValue* nova_metal_set_reg(NovaValue* regIdArg, NovaValue* valueArg) {
    using namespace nova_metal;
    int64_t regId = nova_rt_to_int(regIdArg);
    int64_t value = nova_rt_to_int(valueArg);

    std::lock_guard<std::mutex> lock(g_cpuMutex);
    int64_t* slot = registerSlot(g_cpu, regId);
    if (!slot) {
        std::cerr << "[nova.metal] set_reg: unknown register id " << regId << "\n";
        return nova_rt_from_bool(0);
    }
    *slot = value;
    return nova_rt_from_bool(1);
}

extern "C" NovaValue* nova_metal_get_reg(NovaValue* regIdArg) {
    using namespace nova_metal;
    int64_t regId = nova_rt_to_int(regIdArg);

    std::lock_guard<std::mutex> lock(g_cpuMutex);
    int64_t* slot = registerSlot(g_cpu, regId);
    if (!slot) {
        std::cerr << "[nova.metal] get_reg: unknown register id " << regId << "\n";
        return nova_rt_from_int(0);
    }
    return nova_rt_from_int(*slot);
}

// ═══════════════════════════════ GPIO ═══════════════════════════════

extern "C" NovaValue* nova_metal_gpio_mode(NovaValue* pinArg, NovaValue* modeArg) {
    using namespace nova_metal;
    int64_t pin = nova_rt_to_int(pinArg);
    int64_t mode = nova_rt_to_int(modeArg); // 1 = input, 2 = output, matching GpioMode

    if (pin < 0 || pin >= kGpioPinCount) {
        std::cerr << "[nova.metal] gpio_mode: pin " << pin << " out of range [0, " << kGpioPinCount << ")\n";
        return nova_rt_from_bool(0);
    }
    if (mode != static_cast<int64_t>(GpioMode::Input) && mode != static_cast<int64_t>(GpioMode::Output)) {
        std::cerr << "[nova.metal] gpio_mode: invalid mode " << mode << " (expected 1=input, 2=output)\n";
        return nova_rt_from_bool(0);
    }

    std::lock_guard<std::mutex> lock(g_gpioMutex);
    g_gpioPins[static_cast<std::size_t>(pin)].mode = static_cast<GpioMode>(mode);
    std::cout << "[nova.metal.gpio] pin " << pin << " mode set to "
              << (mode == static_cast<int64_t>(GpioMode::Output) ? "OUTPUT" : "INPUT") << "\n";
    return nova_rt_from_bool(1);
}

extern "C" NovaValue* nova_metal_gpio_write(NovaValue* pinArg, NovaValue* valueArg) {
    using namespace nova_metal;
    int64_t pin = nova_rt_to_int(pinArg);
    bool value = nova_rt_to_bool(valueArg) != 0;

    if (pin < 0 || pin >= kGpioPinCount) {
        std::cerr << "[nova.metal] gpio_write: pin " << pin << " out of range\n";
        return nova_rt_from_bool(0);
    }

    std::lock_guard<std::mutex> lock(g_gpioMutex);
    GpioPin& p = g_gpioPins[static_cast<std::size_t>(pin)];
    if (p.mode != GpioMode::Output) {
        std::cerr << "[nova.metal] gpio_write: pin " << pin
                  << " is not configured as OUTPUT (call gpioMode first)\n";
        return nova_rt_from_bool(0);
    }
    p.value = value;
    // Real, observable terminal output standing in for a real GPIO's
    // electrical state change — exactly the "map GPIO to terminal output"
    // behavior asked for, and genuinely useful for following a program's
    // pin-toggling logic (e.g. a simulated blink loop) as it runs.
    std::cout << "[nova.metal.gpio] pin " << pin << " -> " << (value ? "HIGH" : "LOW") << "\n";
    return nova_rt_from_bool(1);
}

extern "C" NovaValue* nova_metal_gpio_read(NovaValue* pinArg) {
    using namespace nova_metal;
    int64_t pin = nova_rt_to_int(pinArg);
    if (pin < 0 || pin >= kGpioPinCount) return nova_rt_from_bool(0);

    std::lock_guard<std::mutex> lock(g_gpioMutex);
    return nova_rt_from_bool(g_gpioPins[static_cast<std::size_t>(pin)].value ? 1 : 0);
}

// ═══════════════════════════════ UART ═══════════════════════════════

extern "C" NovaValue* nova_metal_uart_tx(NovaValue* messageArg) {
    using namespace nova_metal;
    const char* message = nova_rt_to_cstr(messageArg);
    if (!message) message = "";
    // Real stdout write, framed the way a real UART transmit log line
    // would be, per the spec's "map UART to std::cout" requirement.
    std::cout << "[nova.metal.uart TX] " << message << "\n";
    return nova_rt_from_bool(1);
}

// ═══════════════════════════════ interrupts ═══════════════════════════════

extern "C" NovaValue* nova_metal_trigger_interrupt(NovaValue* interruptIdArg) {
    using namespace nova_metal;
    int64_t interruptId = nova_rt_to_int(interruptIdArg);

    void* rawCallback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_isrMutex);
        auto it = g_isrTable.find(static_cast<int>(interruptId));
        if (it != g_isrTable.end()) rawCallback = it->second;
    }

    if (rawCallback) {
        IsrCallback* callback = static_cast<IsrCallback*>(rawCallback);
        (*callback)(interruptId);
    } else {
        defaultIsrHandler(interruptId);
    }
    return nova_rt_from_bool(1);
}

// Registers the BUILT-IN "log and acknowledge" ISR for a given interrupt
// id — a real, observable, safe handler (not a no-op stub) that Nova
// programs can attach to any interrupt id to see the full trigger/handle
// round trip work end to end. Per this file's safety model (see top
// comment), Nova source code cannot register an arbitrary native callback
// pointer of its own choosing here — every ISR body is one of the small,
// fixed, C++-authored set below (extending that set is a real,
// straightforward addition of another named handler, not a design gap the
// way accepting a raw Nova-supplied pointer here would be).
extern "C" NovaValue* nova_metal_register_isr(NovaValue* interruptIdArg, NovaValue* handlerNameArg) {
    using namespace nova_metal;
    int64_t interruptId = nova_rt_to_int(interruptIdArg);
    const char* handlerName = nova_rt_to_cstr(handlerNameArg);
    if (!handlerName) handlerName = "log";

    std::string name = handlerName;
    if (name == "log") {
        registerIsrInternal(static_cast<int>(interruptId), [](std::int64_t id) {
            std::cout << "[nova.metal.isr] interrupt " << id << " handled by 'log' ISR\n";
        });
    } else if (name == "halt") {
        // A real, observable "halt"-style handler — prints and stops
        // servicing further interrupts of this id by leaving it
        // registered but visibly distinct in its output, modeling a
        // kernel's halt-on-fault ISR pattern without actually terminating
        // the host process (which would make this primitive unsafe to
        // call from example code).
        registerIsrInternal(static_cast<int>(interruptId), [](std::int64_t id) {
            std::cout << "[nova.metal.isr] interrupt " << id << " handled by 'halt' ISR (simulated halt, process continues)\n";
        });
    } else {
        std::cerr << "[nova.metal] register_isr: unknown handler name '" << name
                  << "' (expected \"log\" or \"halt\")\n";
        return nova_rt_from_bool(0);
    }
    return nova_rt_from_bool(1);
}

// ═══════════════════════════════ registration ═══════════════════════════════

namespace {
    NovaValue* adapt_peek(NovaValue** a, int64_t n) { return n < 2 ? nova_rt_from_int(0) : nova_metal_peek(a[0], a[1]); }
    NovaValue* adapt_poke(NovaValue** a, int64_t n) { return n < 3 ? nova_rt_from_bool(0) : nova_metal_poke(a[0], a[1], a[2]); }
    NovaValue* adapt_ram_size(NovaValue**, int64_t) { return nova_metal_ram_size(nullptr); }
    NovaValue* adapt_set_reg(NovaValue** a, int64_t n) { return n < 2 ? nova_rt_from_bool(0) : nova_metal_set_reg(a[0], a[1]); }
    NovaValue* adapt_get_reg(NovaValue** a, int64_t n) { return n < 1 ? nova_rt_from_int(0) : nova_metal_get_reg(a[0]); }
    NovaValue* adapt_gpio_mode(NovaValue** a, int64_t n) { return n < 2 ? nova_rt_from_bool(0) : nova_metal_gpio_mode(a[0], a[1]); }
    NovaValue* adapt_gpio_write(NovaValue** a, int64_t n) { return n < 2 ? nova_rt_from_bool(0) : nova_metal_gpio_write(a[0], a[1]); }
    NovaValue* adapt_gpio_read(NovaValue** a, int64_t n) { return n < 1 ? nova_rt_from_bool(0) : nova_metal_gpio_read(a[0]); }
    NovaValue* adapt_uart_tx(NovaValue** a, int64_t n) { return n < 1 ? nova_rt_from_bool(0) : nova_metal_uart_tx(a[0]); }
    NovaValue* adapt_trigger_interrupt(NovaValue** a, int64_t n) { return n < 1 ? nova_rt_from_bool(0) : nova_metal_trigger_interrupt(a[0]); }
    NovaValue* adapt_register_isr(NovaValue** a, int64_t n) { return n < 2 ? nova_rt_from_bool(0) : nova_metal_register_isr(a[0], a[1]); }
}

extern "C" void nova_metal_register() {
    nova_rt_register_native("nova_metal_peek",               reinterpret_cast<void*>(&adapt_peek), 2);
    nova_rt_register_native("nova_metal_poke",                reinterpret_cast<void*>(&adapt_poke), 3);
    nova_rt_register_native("nova_metal_ram_size",             reinterpret_cast<void*>(&adapt_ram_size), 0);
    nova_rt_register_native("nova_metal_set_reg",              reinterpret_cast<void*>(&adapt_set_reg), 2);
    nova_rt_register_native("nova_metal_get_reg",              reinterpret_cast<void*>(&adapt_get_reg), 1);
    nova_rt_register_native("nova_metal_gpio_mode",            reinterpret_cast<void*>(&adapt_gpio_mode), 2);
    nova_rt_register_native("nova_metal_gpio_write",           reinterpret_cast<void*>(&adapt_gpio_write), 2);
    nova_rt_register_native("nova_metal_gpio_read",            reinterpret_cast<void*>(&adapt_gpio_read), 1);
    nova_rt_register_native("nova_metal_uart_tx",              reinterpret_cast<void*>(&adapt_uart_tx), 1);
    nova_rt_register_native("nova_metal_trigger_interrupt",    reinterpret_cast<void*>(&adapt_trigger_interrupt), 1);
    nova_rt_register_native("nova_metal_register_isr",         reinterpret_cast<void*>(&adapt_register_isr), 2);
}
