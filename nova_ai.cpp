// nova_ai.cpp - Nova AI/ML tensor backend (C++17)
//
// ABI note: nova_bridge.cpp dispatches registered natives through the generic
// signature NovaValue* (*)(NovaValue**, long). The typed functions below are
// the actual AI API; small ABI adapters are registered so the bridge remains
// type-safe with respect to its current dispatcher contract.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

extern "C" {

struct NovaValue {
    std::int32_t type;
    std::int32_t padding;
    long i;
    double f;
    char* s;
};

NovaValue* nova_rt_from_int(long value);
long nova_rt_to_int(NovaValue* value);
NovaValue* nova_rt_from_float(double value);
double nova_rt_to_float(NovaValue* value);
void nova_rt_register_native(const char* name, void* fnPtr, int arity);

}

namespace {

// Nova's current bridge uses type=1 for integers and type=2 for floats.
constexpr std::int32_t NOVA_INT = 1;
constexpr std::int32_t NOVA_FLOAT = 2;

struct Tensor {
    std::size_t rows;
    std::size_t cols;
    std::vector<double> data;
    mutable std::mutex mutex;

    Tensor(std::size_t r, std::size_t c) : rows(r), cols(c), data(r * c, 0.0) {}

    double& at(std::size_t r, std::size_t c) noexcept {
        return data[r * cols + c];
    }
    const double& at(std::size_t r, std::size_t c) const noexcept {
        return data[r * cols + c];
    }
};

std::vector<Tensor*> g_registry;
std::mutex g_registry_mutex;

NovaValue* make_int(long value) {
    return nova_rt_from_int(value);
}

NovaValue* make_float(double value) {
    return nova_rt_from_float(value);
}

bool read_integer(NovaValue* value, long& out) {
    if (!value || value->type != NOVA_INT) return false;
    out = nova_rt_to_int(value);
    return true;
}

bool read_number(NovaValue* value, double& out) {
    if (!value) return false;
    if (value->type == NOVA_FLOAT) {
        out = nova_rt_to_float(value);
        return true;
    }
    if (value->type == NOVA_INT) {
        out = static_cast<double>(nova_rt_to_int(value));
        return true;
    }
    return false;
}

Tensor* lookup_tensor(long id) {
    if (id < 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    const auto index = static_cast<std::size_t>(id);
    if (index >= g_registry.size()) return nullptr;
    return g_registry[index];
}

bool checked_size(long value, std::size_t& out) {
    if (value <= 0) return false;
    const auto n = static_cast<std::uintmax_t>(value);
    if (n > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) return false;
    out = static_cast<std::size_t>(n);
    return true;
}

bool checked_tensor_elements(std::size_t rows, std::size_t cols) {
    return rows != 0 && cols != 0 && rows <= std::numeric_limits<std::size_t>::max() / cols;
}

} // namespace

// ---------------------------------------------------------------------------
// Typed AI API
// ---------------------------------------------------------------------------

extern "C" NovaValue* nova_ai_create(NovaValue* rowsValue, NovaValue* colsValue) {
    long rowsLong = 0;
    long colsLong = 0;
    if (!read_integer(rowsValue, rowsLong) || !read_integer(colsValue, colsLong)) return nullptr;

    std::size_t rows = 0;
    std::size_t cols = 0;
    if (!checked_size(rowsLong, rows) || !checked_size(colsLong, cols)) return nullptr;
    if (!checked_tensor_elements(rows, cols)) return nullptr;

    Tensor* tensor = nullptr;
    try {
        tensor = new Tensor(rows, cols);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }

    try {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        if (g_registry.size() >= static_cast<std::size_t>(std::numeric_limits<long>::max())) {
            delete tensor;
            return nullptr;
        }
        const long id = static_cast<long>(g_registry.size());
        g_registry.push_back(tensor);
        return make_int(id);
    } catch (...) {
        delete tensor;
        return nullptr;
    }
}

extern "C" NovaValue* nova_ai_set(NovaValue* idValue,
                                   NovaValue* rowValue,
                                   NovaValue* colValue,
                                   NovaValue* value) {
    long id = 0;
    long rowLong = 0;
    long colLong = 0;
    double number = 0.0;

    if (!read_integer(idValue, id) || !read_integer(rowValue, rowLong) ||
        !read_integer(colValue, colLong) || !read_number(value, number)) {
        return nullptr;
    }
    if (rowLong < 0 || colLong < 0) return nullptr;

    Tensor* tensor = lookup_tensor(id);
    if (!tensor) return nullptr;

    const auto row = static_cast<std::size_t>(rowLong);
    const auto col = static_cast<std::size_t>(colLong);
    {
        std::lock_guard<std::mutex> lock(tensor->mutex);
        if (row >= tensor->rows || col >= tensor->cols) return nullptr;
        tensor->at(row, col) = number;
    }

    return make_float(number);
}

extern "C" NovaValue* nova_ai_get(NovaValue* idValue,
                                   NovaValue* rowValue,
                                   NovaValue* colValue) {
    long id = 0;
    long rowLong = 0;
    long colLong = 0;

    if (!read_integer(idValue, id) || !read_integer(rowValue, rowLong) ||
        !read_integer(colValue, colLong)) {
        return nullptr;
    }
    if (rowLong < 0 || colLong < 0) return nullptr;

    Tensor* tensor = lookup_tensor(id);
    if (!tensor) return nullptr;

    const auto row = static_cast<std::size_t>(rowLong);
    const auto col = static_cast<std::size_t>(colLong);
    double result = 0.0;
    {
        std::lock_guard<std::mutex> lock(tensor->mutex);
        if (row >= tensor->rows || col >= tensor->cols) return nullptr;
        result = tensor->at(row, col);
    }

    return make_float(result);
}

extern "C" NovaValue* nova_ai_matmul(NovaValue* idAValue, NovaValue* idBValue) {
    long idALong = 0;
    long idBLong = 0;
    if (!read_integer(idAValue, idALong) || !read_integer(idBValue, idBLong)) return nullptr;

    Tensor* a = lookup_tensor(idALong);
    Tensor* b = lookup_tensor(idBLong);
    if (!a || !b) return nullptr;

    std::size_t outRows = 0;
    std::size_t outCols = 0;
    std::size_t shared = 0;

    if (a == b) {
        std::lock_guard<std::mutex> lock(a->mutex);
        if (a->rows != a->cols) return nullptr;
        outRows = a->rows;
        outCols = a->cols;
        shared = a->cols;

        if (!checked_tensor_elements(outRows, outCols)) return nullptr;
        Tensor* result = nullptr;
        try {
            result = new Tensor(outRows, outCols);
        } catch (const std::bad_alloc&) {
            return nullptr;
        }

        // i-k-j ordering keeps the left-hand scalar hot and streams across
        // the right-hand row and result row, which is substantially faster
        // than the naive i-j-k form for the row-major storage used here.
        for (std::size_t i = 0; i < outRows; ++i) {
            double* cRow = result->data.data() + i * outCols;
            const double* aRow = a->data.data() + i * a->cols;
            for (std::size_t k = 0; k < shared; ++k) {
                const double aik = aRow[k];
                const double* bRow = a->data.data() + k * a->cols;
                for (std::size_t j = 0; j < outCols; ++j) {
                    cRow[j] += aik * bRow[j];
                }
            }
        }

        try {
            std::lock_guard<std::mutex> registryLock(g_registry_mutex);
            if (g_registry.size() >= static_cast<std::size_t>(std::numeric_limits<long>::max())) {
                delete result;
                return nullptr;
            }
            const long id = static_cast<long>(g_registry.size());
            g_registry.push_back(result);
            return make_int(id);
        } catch (...) {
            delete result;
            return nullptr;
        }
    }

    // Lock both input tensors for a consistent snapshot. std::scoped_lock
    // prevents lock-order deadlocks when two threads multiply the same pair
    // in opposite order.
    std::scoped_lock inputLocks(a->mutex, b->mutex);
    if (a->cols != b->rows) return nullptr;

    outRows = a->rows;
    outCols = b->cols;
    shared = a->cols;
    if (!checked_tensor_elements(outRows, outCols)) return nullptr;

    Tensor* result = nullptr;
    try {
        result = new Tensor(outRows, outCols);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }

    // Raw O(n^3) dot-product multiplication. i-k-j gives contiguous writes
    // to C and contiguous reads from B's rows, improving cache locality.
    for (std::size_t i = 0; i < outRows; ++i) {
        double* cRow = result->data.data() + i * outCols;
        const double* aRow = a->data.data() + i * a->cols;
        for (std::size_t k = 0; k < shared; ++k) {
            const double aik = aRow[k];
            const double* bRow = b->data.data() + k * b->cols;
            for (std::size_t j = 0; j < outCols; ++j) {
                cRow[j] += aik * bRow[j];
            }
        }
    }

    try {
        std::lock_guard<std::mutex> registryLock(g_registry_mutex);
        if (g_registry.size() >= static_cast<std::size_t>(std::numeric_limits<long>::max())) {
            delete result;
            return nullptr;
        }
        const long id = static_cast<long>(g_registry.size());
        g_registry.push_back(result);
        return make_int(id);
    } catch (...) {
        delete result;
        return nullptr;
    }
}

extern "C" NovaValue* nova_ai_relu(NovaValue* idValue) {
    long id = 0;
    if (!read_integer(idValue, id)) return nullptr;

    Tensor* tensor = lookup_tensor(id);
    if (!tensor) return nullptr;

    std::lock_guard<std::mutex> lock(tensor->mutex);
    for (double& value : tensor->data) {
        if (value < 0.0) value = 0.0;
    }
    return make_int(id);
}

// ---------------------------------------------------------------------------
// Current Nova bridge ABI adapters
// ---------------------------------------------------------------------------
// nova_rt_call() currently casts every registered function to:
//     NovaValue* (*)(NovaValue**, long)
// Therefore registering the typed API functions directly would be undefined
// behaviour. These adapters validate arity and then invoke the typed API.

extern "C" NovaValue* nova_ai_create_native(NovaValue** args, long argc) {
    return (argc == 2 && args) ? nova_ai_create(args[0], args[1]) : nullptr;
}

extern "C" NovaValue* nova_ai_set_native(NovaValue** args, long argc) {
    return (argc == 4 && args) ? nova_ai_set(args[0], args[1], args[2], args[3]) : nullptr;
}

extern "C" NovaValue* nova_ai_get_native(NovaValue** args, long argc) {
    return (argc == 3 && args) ? nova_ai_get(args[0], args[1], args[2]) : nullptr;
}

extern "C" NovaValue* nova_ai_matmul_native(NovaValue** args, long argc) {
    return (argc == 2 && args) ? nova_ai_matmul(args[0], args[1]) : nullptr;
}

extern "C" NovaValue* nova_ai_relu_native(NovaValue** args, long argc) {
    return (argc == 1 && args) ? nova_ai_relu(args[0]) : nullptr;
}

extern "C" void nova_ai_register() {
    nova_rt_register_native("nova_ai_create", reinterpret_cast<void*>(&nova_ai_create_native), 2);
    nova_rt_register_native("nova_ai_set", reinterpret_cast<void*>(&nova_ai_set_native), 4);
    nova_rt_register_native("nova_ai_get", reinterpret_cast<void*>(&nova_ai_get_native), 3);
    nova_rt_register_native("nova_ai_matmul", reinterpret_cast<void*>(&nova_ai_matmul_native), 2);
    nova_rt_register_native("nova_ai_relu", reinterpret_cast<void*>(&nova_ai_relu_native), 1);
}
