// nova_ffi.cpp
// Nova MAP 17: Universal FFI core.
//
// ABI assumptions (matching nova_bridge.cpp / nova_rt.cpp):
//   struct NovaValue {
//       int32_t type;
//       int32_t padding;
//       long i;
//       double f;
//       char* s;
//   };
//   type 1 = Int, 2 = Float, 3 = String.
// This backend additionally uses private type 4 for native pointers/handles.
//
// Build (Linux/Android):
//   clang++ -std=c++17 -fPIC -c nova_ffi.cpp
//   ... -ldl -lffi
//
// Build (macOS):
//   clang++ -std=c++17 -fPIC -c nova_ffi.cpp
//   ... -lffi
//
// Windows:
//   link against libffi and use LoadLibrary/GetProcAddress.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include <ffi.h>

#if defined(_WIN32)
#  define NOMINMAX
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

extern "C" {

struct NovaValue {
    int32_t type;
    int32_t padding;
    long i;
    double f;
    char* s;
};

// Runtime boxing/unboxing API supplied by nova_rt.cpp / nova_bridge.cpp.
NovaValue* nova_rt_from_int(long v);
NovaValue* nova_rt_from_float(double v);
NovaValue* nova_rt_from_string(const char* s);
long        nova_rt_to_int(NovaValue* v);
double      nova_rt_to_float(NovaValue* v);
const char* nova_rt_to_cstr(NovaValue* v);

// Native registration API supplied by the Nova runtime.
void nova_rt_register_native(const char* name, void* fnPtr, int arity);

} // extern "C"

namespace {

constexpr int32_t NOVA_INT    = 1;
constexpr int32_t NOVA_FLOAT  = 2;
constexpr int32_t NOVA_STRING = 3;
constexpr int32_t NOVA_PTR    = 4; // private FFI pointer/handle value

struct LibraryEntry {
#if defined(_WIN32)
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
};

std::vector<LibraryEntry*> g_libraries;
std::mutex g_libraries_mutex;

// FFI pointer values are deliberately boxed in a NovaValue with private
// type 4. The existing NovaValue ABI has no public pointer variant.
NovaValue* make_pointer_value(void* p) {
    if (!p) return nullptr;

    NovaValue* v = new NovaValue{};
    v->type = NOVA_PTR;
    v->padding = 0;
    v->i = static_cast<long>(
        reinterpret_cast<std::uintptr_t>(p)
    );
    v->f = 0.0;
    v->s = nullptr;
    return v;
}

void* pointer_from_value(NovaValue* v) {
    if (!v || v->type != NOVA_PTR) return nullptr;
    return reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(v->i)
    );
}

bool get_int64(NovaValue* v, std::int64_t& out) {
    if (!v || v->type != NOVA_INT) return false;
    out = static_cast<std::int64_t>(v->i);
    return true;
}

bool get_double(NovaValue* v, double& out) {
    if (!v || v->type != NOVA_FLOAT) return false;
    out = v->f;
    return true;
}

bool valid_dimension(std::int64_t value) {
    return value >= 0 &&
           value <= static_cast<std::int64_t>(
               std::numeric_limits<std::size_t>::max());
}

LibraryEntry* library_from_id(NovaValue* v) {
    std::int64_t id64 = 0;
    if (!get_int64(v, id64) || id64 <= 0) return nullptr;

    std::lock_guard<std::mutex> lock(g_libraries_mutex);
    const std::size_t index = static_cast<std::size_t>(id64 - 1);
    if (index >= g_libraries.size()) return nullptr;
    return g_libraries[index];
}

ffi_type* ffi_type_for_argument(NovaValue* value) {
    if (!value) return nullptr;

    switch (value->type) {
        case NOVA_INT:
            return &ffi_type_sint64;
        case NOVA_FLOAT:
            return &ffi_type_double;
        case NOVA_STRING:
        case NOVA_PTR:
            return &ffi_type_pointer;
        default:
            return nullptr;
    }
}

bool encode_argument(NovaValue* value, ffi_type* type, void* storage) {
    if (!value || !type || !storage) return false;

    switch (value->type) {
        case NOVA_INT: {
            std::int64_t x = static_cast<std::int64_t>(value->i);
            std::memcpy(storage, &x, sizeof(x));
            return true;
        }

        case NOVA_FLOAT: {
            double x = value->f;
            std::memcpy(storage, &x, sizeof(x));
            return true;
        }

        case NOVA_STRING: {
            const char* s = value->s ? value->s : "";
            std::memcpy(storage, &s, sizeof(s));
            return true;
        }

        case NOVA_PTR: {
            void* p = pointer_from_value(value);
            std::memcpy(storage, &p, sizeof(p));
            return true;
        }

        default:
            return false;
    }
}

NovaValue* box_return_value(NovaValue* return_type,
                            const unsigned char* raw,
                            std::size_t raw_size) {
    if (!return_type || !raw) return nullptr;

    switch (return_type->type) {
        case NOVA_INT: {
            std::int64_t value = 0;
            std::memcpy(&value, raw, sizeof(value));
            return nova_rt_from_int(static_cast<long>(value));
        }

        case NOVA_FLOAT: {
            double value = 0.0;
            std::memcpy(&value, raw, sizeof(value));
            return nova_rt_from_float(value);
        }

        case NOVA_STRING: {
            char* value = nullptr;
            std::memcpy(&value, raw, sizeof(value));
            if (!value) return nullptr;
            return nova_rt_from_string(value);
        }

        case NOVA_PTR: {
            void* value = nullptr;
            std::memcpy(&value, raw, sizeof(value));
            return make_pointer_value(value);
        }

        default:
            return nullptr;
    }

    (void)raw_size;
}

} // namespace

extern "C" {

// ---------------------------------------------------------------------------
// 1. Dynamic library loading
// ---------------------------------------------------------------------------
//
// Returns a positive integer library ID. The ID is stored in a Nova Int.
// LibraryEntry objects are intentionally retained for the lifetime of the
// process because this first MAP 17 core has no unload primitive; that avoids
// dangling function pointers after a library is closed.

NovaValue* nova_ffi_load_library(NovaValue* path) {
    if (!path || path->type != NOVA_STRING || !path->s || !*path->s)
        return nullptr;

    auto* entry = new LibraryEntry{};

#if defined(_WIN32)
    entry->handle = LoadLibraryA(path->s);
    if (!entry->handle) {
        delete entry;
        return nullptr;
    }
#else
    entry->handle = dlopen(path->s, RTLD_NOW | RTLD_LOCAL);
    if (!entry->handle) {
        delete entry;
        return nullptr;
    }
#endif

    std::lock_guard<std::mutex> lock(g_libraries_mutex);
    g_libraries.push_back(entry);

    // Registry index is 0-based; Nova-visible IDs are 1-based.
    const std::int64_t id =
        static_cast<std::int64_t>(g_libraries.size());

    return nova_rt_from_int(static_cast<long>(id));
}

// ---------------------------------------------------------------------------
// 2. Symbol resolution
// ---------------------------------------------------------------------------
//
// Returns a private Nova pointer value (type 4). The pointer is the address
// returned by dlsym/GetProcAddress and is suitable for libffi invocation.

NovaValue* nova_ffi_get_symbol(NovaValue* libHandle,
                               NovaValue* symbolName) {
    LibraryEntry* entry = library_from_id(libHandle);
    if (!entry ||
        !symbolName ||
        symbolName->type != NOVA_STRING ||
        !symbolName->s ||
        !*symbolName->s) {
        return nullptr;
    }

#if defined(_WIN32)
    FARPROC proc = GetProcAddress(entry->handle, symbolName->s);
    return make_pointer_value(
        reinterpret_cast<void*>(proc)
    );
#else
    // POSIX dlsym returns void*. POSIX permits conversion of the result to a
    // callable symbol address; libffi consumes it as a code pointer.
    void* symbol = dlsym(entry->handle, symbolName->s);
    return make_pointer_value(symbol);
#endif
}

// ---------------------------------------------------------------------------
// 3. Universal libffi invoker
// ---------------------------------------------------------------------------
//
// ABI:
//   funcPtr   = Nova private pointer value (type 4)
//   returnType:
//       Int    (1) -> ffi_type_sint64
//       Float  (2) -> ffi_type_double
//       String (3) -> ffi_type_pointer
//       Ptr    (4) -> ffi_type_pointer
//   args:
//       Int    -> ffi_type_sint64
//       Float  -> ffi_type_double
//       String -> ffi_type_pointer (char const*)
//       Ptr    -> ffi_type_pointer
//
// The function deliberately accepts only a finite, well-defined ABI type
// surface. That is the safe foundation for MAP 17; structs, variadics,
// callbacks, vector registers, and custom calling conventions belong in a
// later ABI-description layer.

NovaValue* nova_ffi_invoke(NovaValue* funcPtr,
                           NovaValue* returnType,
                           NovaValue** args,
                           int argc) {
    void* function = pointer_from_value(funcPtr);
    if (!function || !returnType || argc < 0)
        return nullptr;

    if (argc > 4096)
        return nullptr;

    ffi_type* ffi_return = nullptr;
    switch (returnType->type) {
        case NOVA_INT:
            ffi_return = &ffi_type_sint64;
            break;
        case NOVA_FLOAT:
            ffi_return = &ffi_type_double;
            break;
        case NOVA_STRING:
        case NOVA_PTR:
            ffi_return = &ffi_type_pointer;
            break;
        default:
            return nullptr;
    }

    std::vector<ffi_type*> arg_types(
        static_cast<std::size_t>(argc), nullptr);

    // Each argument has its own aligned native storage. A union is sufficient
    // for this restricted ABI and avoids type-punning/unaligned accesses.
    union ArgStorage {
        std::int64_t i64;
        double f64;
        void* ptr;
    };

    std::vector<ArgStorage> storage(
        static_cast<std::size_t>(argc));

    std::vector<void*> arg_values(
        static_cast<std::size_t>(argc), nullptr);

    for (int i = 0; i < argc; ++i) {
        NovaValue* value = args ? args[i] : nullptr;
        arg_types[static_cast<std::size_t>(i)] =
            ffi_type_for_argument(value);

        if (!arg_types[static_cast<std::size_t>(i)])
            return nullptr;

        void* slot = &storage[static_cast<std::size_t>(i)];
        if (!encode_argument(value,
                             arg_types[static_cast<std::size_t>(i)],
                             slot)) {
            return nullptr;
        }

        arg_values[static_cast<std::size_t>(i)] = slot;
    }

    ffi_cif cif{};
    ffi_status status = ffi_prep_cif(
        &cif,
        FFI_DEFAULT_ABI,
        static_cast<unsigned int>(argc),
        ffi_return,
        arg_types.empty() ? nullptr : arg_types.data());

    if (status != FFI_OK)
        return nullptr;

    // ffi_call writes the result according to the selected ffi_type.
    // Use pointer-sized storage because all supported return values are
    // <= sizeof(void*) except double/sint64, which are also 8 bytes on the
    // supported 64-bit targets.
    alignas(std::max_align_t)
    unsigned char raw_return[sizeof(double)] = {};

    ffi_call(
        &cif,
        FFI_FN(function),
        raw_return,
        arg_values.empty() ? nullptr : arg_values.data());

    return box_return_value(
        returnType,
        raw_return,
        sizeof(raw_return));
}

// ---------------------------------------------------------------------------
// Registration adapters
// ---------------------------------------------------------------------------
//
// nova_rt_register_native() in the current Nova runtime registers functions
// with the generic dispatch ABI:
//
//   NovaValue* fn(NovaValue** args, long argc)
//
// Keep the requested typed APIs above as the real implementation and expose
// these tiny adapters to the dynamic Nova dispatcher.

static NovaValue* ffi_load_adapter(NovaValue** args, long argc) {
    if (argc != 1 || !args) return nullptr;
    return nova_ffi_load_library(args[0]);
}

static NovaValue* ffi_symbol_adapter(NovaValue** args, long argc) {
    if (argc != 2 || !args) return nullptr;
    return nova_ffi_get_symbol(args[0], args[1]);
}

static NovaValue* ffi_invoke_adapter(NovaValue** args, long argc) {
    if (argc < 2 || !args ||
        argc - 2 > static_cast<long>(std::numeric_limits<int>::max())) {
        return nullptr;
    }

    NovaValue* function = args[0];
    NovaValue* returnType = args[1];
    NovaValue** callArgs = (argc > 2) ? &args[2] : nullptr;
    return nova_ffi_invoke(
        function,
        returnType,
        callArgs,
        static_cast<int>(argc - 2));
}

void nova_ffi_register() {
    nova_rt_register_native(
        "nova_ffi_load_library",
        reinterpret_cast<void*>(&ffi_load_adapter),
        1);

    nova_rt_register_native(
        "nova_ffi_get_symbol",
        reinterpret_cast<void*>(&ffi_symbol_adapter),
        2);

    // Arity is -1 because Nova's generic native dispatcher accepts a dynamic
    // argument count for this universal invoker.
    nova_rt_register_native(
        "nova_ffi_invoke",
        reinterpret_cast<void*>(&ffi_invoke_adapter),
        -1);
}

} // extern "C"
