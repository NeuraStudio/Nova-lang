// nova_rt.cpp — Nova Language Core Runtime Engine Implementation
// C++17. Real dynamic semantics: refcounted heap values, boxing/unboxing,
// collections, coercion, and a thread-based generator/async model.

#include "nova_rt.hpp"

#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// ============================================================================
// Internal heap payload — hidden behind the opaque NovaObject* in the ABI.
// ============================================================================
struct NovaObject {
    NovaType kind;
    std::string str;                                     // STRING
    std::vector<NovaValue*> arr;                          // ARRAY / TUPLE
    std::vector<std::pair<NovaValue*, NovaValue*>> map;   // MAP (linear scan; V2: hash index)
    NovaValue*  cell = nullptr;                            // REF
    std::string errMsg;                                    // ERROR

    // Generator/coroutine coordination (thread-based, see nova_rt_yield).
    std::mutex               gmu;
    std::condition_variable  gcv;
    std::thread              gthread;
    bool       gWantRun  = false;
    bool       gHasValue = false;
    bool       gDone     = false;
    NovaValue* gPending  = nullptr;
};

static thread_local NovaObject* t_currentGenerator = nullptr;
static std::mutex g_rcMutex; // coarse-grained: correctness first, V2 = atomic/sharded refcounts

static NovaValue* new_value(NovaType t) {
    NovaValue* v = new NovaValue();
    v->type = t;
    v->refcount = 1;
    v->as.i = 0;
    return v;
}
static NovaValue* new_object_value(NovaType t) {
    NovaValue* v = new_value(t);
    v->as.obj = new NovaObject();
    v->as.obj->kind = t;
    return v;
}
static bool is_numeric(NovaValue* v) {
    return v && (v->type == NOVA_INT || v->type == NOVA_FLOAT || v->type == NOVA_BOOL);
}

// ============================================================================
// Reference Counting
// ============================================================================
NovaValue* nova_rt_retain(NovaValue* v) {
    if (!v) return v;
    std::lock_guard<std::mutex> lk(g_rcMutex);
    v->refcount++;
    return v;
}

static void release_object(NovaObject* o) {
    if (!o) return;
    for (auto* e : o->arr) nova_rt_release(e);
    for (auto& kv : o->map) { nova_rt_release(kv.first); nova_rt_release(kv.second); }
    if (o->cell) nova_rt_release(o->cell);
    if (o->gPending) nova_rt_release(o->gPending);
    if (o->gthread.joinable()) {
        { std::lock_guard<std::mutex> lk(o->gmu); o->gDone = true; o->gWantRun = true; }
        o->gcv.notify_all();
        if (o == t_currentGenerator) o->gthread.detach(); // avoid self-join deadlock
        else o->gthread.join();
    }
    delete o;
}

void nova_rt_release(NovaValue* v) {
    if (!v) return;
    bool hitZero;
    { std::lock_guard<std::mutex> lk(g_rcMutex); hitZero = (--v->refcount <= 0); }
    if (!hitZero) return;
    switch (v->type) {
        case NOVA_STRING: case NOVA_ARRAY: case NOVA_MAP: case NOVA_TUPLE:
        case NOVA_ERROR: case NOVA_REF: case NOVA_FUTURE: case NOVA_GENERATOR:
            release_object(v->as.obj);
            break;
        default: break;
    }
    delete v;
}

// ============================================================================
// Memory / Ref Cells
// ============================================================================
NovaValue* nova_rt_alloc(int64_t size) {
    (void)size; // reserved for future raw-buffer sizing in unsafe mode
    NovaValue* r = new_object_value(NOVA_REF);
    r->as.obj->cell = nova_rt_const_null();
    return r;
}
NovaValue* nova_rt_load(NovaValue* addr) {
    if (!addr || addr->type != NOVA_REF) return nova_rt_const_null();
    return nova_rt_retain(addr->as.obj->cell);
}
void nova_rt_store(NovaValue* addr, NovaValue* value) {
    if (!addr || addr->type != NOVA_REF) return;
    NovaValue* old = addr->as.obj->cell;
    addr->as.obj->cell = nova_rt_retain(value);
    nova_rt_release(old);
}

// ============================================================================
// Constant Boxing / Coercion
// ============================================================================
NovaValue* nova_rt_const_int(int64_t v)  { auto* r = new_value(NOVA_INT);   r->as.i = v; return r; }
NovaValue* nova_rt_const_float(double v) { auto* r = new_value(NOVA_FLOAT); r->as.f = v; return r; }
NovaValue* nova_rt_const_bool(bool v)    { auto* r = new_value(NOVA_BOOL);  r->as.b = v; return r; }
NovaValue* nova_rt_const_null(void)      { return new_value(NOVA_NULL); }
NovaValue* nova_rt_const_string(const char* v) {
    auto* r = new_object_value(NOVA_STRING);
    r->as.obj->str = v ? v : "";
    return r;
}
NovaValue* nova_rt_from_int(int64_t v)   { return nova_rt_const_int(v); }
NovaValue* nova_rt_from_float(double v)  { return nova_rt_const_float(v); }
NovaValue* nova_rt_from_bool(bool v)     { return nova_rt_const_bool(v); }
NovaValue* nova_rt_from_string(const char* v) { return nova_rt_const_string(v); }

int64_t nova_rt_to_int(NovaValue* v) {
    if (!v) return 0;
    switch (v->type) {
        case NOVA_INT: return v->as.i;
        case NOVA_FLOAT: return (int64_t)v->as.f;
        case NOVA_BOOL: return v->as.b ? 1 : 0;
        case NOVA_STRING: try { return std::stoll(v->as.obj->str); } catch (...) { return 0; }
        default: return 0;
    }
}
double nova_rt_to_float(NovaValue* v) {
    if (!v) return 0.0;
    switch (v->type) {
        case NOVA_INT: return (double)v->as.i;
        case NOVA_FLOAT: return v->as.f;
        case NOVA_BOOL: return v->as.b ? 1.0 : 0.0;
        case NOVA_STRING: try { return std::stod(v->as.obj->str); } catch (...) { return 0.0; }
        default: return 0.0;
    }
}
bool nova_rt_to_bool(NovaValue* v) {
    if (!v) return false;
    switch (v->type) {
        case NOVA_NULL: return false;
        case NOVA_BOOL: return v->as.b;
        case NOVA_INT: return v->as.i != 0;
        case NOVA_FLOAT: return v->as.f != 0.0;
        case NOVA_STRING: return !v->as.obj->str.empty();
        case NOVA_ARRAY: case NOVA_TUPLE: return !v->as.obj->arr.empty();
        case NOVA_MAP: return !v->as.obj->map.empty();
        case NOVA_ERROR: return false;
        default: return true;
    }
}
const char* nova_rt_to_cstr(NovaValue* v) {
    return (v && v->type == NOVA_STRING) ? v->as.obj->str.c_str() : "";
}

static bool value_equal(NovaValue* a, NovaValue* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (is_numeric(a) && is_numeric(b)) return nova_rt_to_float(a) == nova_rt_to_float(b);
    if (a->type != b->type) return false;
    switch (a->type) {
        case NOVA_NULL: return true;
        case NOVA_STRING: return a->as.obj->str == b->as.obj->str;
        case NOVA_ARRAY: case NOVA_TUPLE: {
            auto &x = a->as.obj->arr, &y = b->as.obj->arr;
            if (x.size() != y.size()) return false;
            for (size_t i = 0; i < x.size(); ++i) if (!value_equal(x[i], y[i])) return false;
            return true;
        }
        case NOVA_MAP: {
            auto &x = a->as.obj->map, &y = b->as.obj->map;
            if (x.size() != y.size()) return false;
            for (auto& kv : x) {
                bool found = false;
                for (auto& kv2 : y) if (value_equal(kv.first, kv2.first) && value_equal(kv.second, kv2.second)) { found = true; break; }
                if (!found) return false;
            }
            return true;
        }
        default: return false;
    }
}

NovaValue* nova_rt_to_string(NovaValue* v) {
    if (!v) return nova_rt_const_string("null");
    switch (v->type) {
        case NOVA_NULL: return nova_rt_const_string("null");
        case NOVA_BOOL: return nova_rt_const_string(v->as.b ? "true" : "false");
        case NOVA_INT: return nova_rt_const_string(std::to_string(v->as.i).c_str());
        case NOVA_FLOAT: return nova_rt_const_string(std::to_string(v->as.f).c_str());
        case NOVA_STRING: return nova_rt_retain(v);
        case NOVA_ERROR: return nova_rt_const_string(("Error: " + v->as.obj->errMsg).c_str());
        case NOVA_ARRAY: case NOVA_TUPLE: {
            std::string out = (v->type == NOVA_ARRAY) ? "[" : "(";
            auto& a = v->as.obj->arr;
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) out += ", ";
                NovaValue* s = nova_rt_to_string(a[i]);
                out += (a[i] && a[i]->type == NOVA_STRING) ? ("\"" + s->as.obj->str + "\"") : s->as.obj->str;
                nova_rt_release(s);
            }
            out += (v->type == NOVA_ARRAY) ? "]" : ")";
            return nova_rt_const_string(out.c_str());
        }
        case NOVA_MAP: {
            std::string out = "{";
            auto& m = v->as.obj->map;
            for (size_t i = 0; i < m.size(); ++i) {
                if (i) out += ", ";
                NovaValue* ks = nova_rt_to_string(m[i].first);
                NovaValue* vs = nova_rt_to_string(m[i].second);
                out += ks->as.obj->str + ": " + vs->as.obj->str;
                nova_rt_release(ks); nova_rt_release(vs);
            }
            out += "}";
            return nova_rt_const_string(out.c_str());
        }
        case NOVA_REF: return nova_rt_const_string("<ref>");
        case NOVA_FUTURE: return nova_rt_const_string("<future>");
        case NOVA_GENERATOR: return nova_rt_const_string("<generator>");
        default: return nova_rt_const_string("<value>");
    }
}

NovaValue* nova_rt_cast(NovaValue* v, const char* typeName) {
    if (!typeName) return nova_rt_retain(v);
    std::string t = typeName;
    if (t == "int") return nova_rt_const_int(nova_rt_to_int(v));
    if (t == "float") return nova_rt_const_float(nova_rt_to_float(v));
    if (t == "bool") return nova_rt_const_bool(nova_rt_to_bool(v));
    if (t == "string") return nova_rt_to_string(v);
    return nova_rt_make_error(("TypeError: unknown cast target '" + t + "'").c_str());
}

// ============================================================================
// Math & Logic
// ============================================================================
NovaValue* nova_rt_add(NovaValue* a, NovaValue* b) {
    if (a && a->type == NOVA_STRING) {
        NovaValue* rhs = nova_rt_to_string(b);
        std::string out = a->as.obj->str + rhs->as.obj->str;
        nova_rt_release(rhs);
        return nova_rt_const_string(out.c_str());
    }
    if (a && b && a->type == NOVA_ARRAY && b->type == NOVA_ARRAY) {
        NovaValue* r = new_object_value(NOVA_ARRAY);
        for (auto* e : a->as.obj->arr) r->as.obj->arr.push_back(nova_rt_retain(e));
        for (auto* e : b->as.obj->arr) r->as.obj->arr.push_back(nova_rt_retain(e));
        return r;
    }
    if (is_numeric(a) && is_numeric(b)) {
        if (a->type == NOVA_FLOAT || b->type == NOVA_FLOAT) return nova_rt_const_float(nova_rt_to_float(a) + nova_rt_to_float(b));
        return nova_rt_const_int(nova_rt_to_int(a) + nova_rt_to_int(b));
    }
    return nova_rt_make_error("TypeError: unsupported operand types for +");
}
NovaValue* nova_rt_sub(NovaValue* a, NovaValue* b) {
    if (!is_numeric(a) || !is_numeric(b)) return nova_rt_make_error("TypeError: unsupported operand types for -");
    if (a->type == NOVA_FLOAT || b->type == NOVA_FLOAT) return nova_rt_const_float(nova_rt_to_float(a) - nova_rt_to_float(b));
    return nova_rt_const_int(nova_rt_to_int(a) - nova_rt_to_int(b));
}
NovaValue* nova_rt_mul(NovaValue* a, NovaValue* b) {
    if (a && a->type == NOVA_STRING && b && (b->type == NOVA_INT || b->type == NOVA_BOOL)) {
        int64_t n = nova_rt_to_int(b);
        std::string out;
        for (int64_t i = 0; i < n; ++i) out += a->as.obj->str;
        return nova_rt_const_string(out.c_str());
    }
    if (!is_numeric(a) || !is_numeric(b)) return nova_rt_make_error("TypeError: unsupported operand types for *");
    if (a->type == NOVA_FLOAT || b->type == NOVA_FLOAT) return nova_rt_const_float(nova_rt_to_float(a) * nova_rt_to_float(b));
    return nova_rt_const_int(nova_rt_to_int(a) * nova_rt_to_int(b));
}
NovaValue* nova_rt_div(NovaValue* a, NovaValue* b) {
    if (!is_numeric(a) || !is_numeric(b)) return nova_rt_make_error("TypeError: unsupported operand types for /");
    double db = nova_rt_to_float(b);
    if (db == 0.0) return nova_rt_make_error("ZeroDivisionError: division by zero");
    return nova_rt_const_float(nova_rt_to_float(a) / db);
}
NovaValue* nova_rt_mod(NovaValue* a, NovaValue* b) {
    if (!is_numeric(a) || !is_numeric(b)) return nova_rt_make_error("TypeError: unsupported operand types for %");
    if (a->type == NOVA_FLOAT || b->type == NOVA_FLOAT) {
        double db = nova_rt_to_float(b);
        if (db == 0.0) return nova_rt_make_error("ZeroDivisionError: modulo by zero");
        return nova_rt_const_float(std::fmod(nova_rt_to_float(a), db));
    }
    int64_t ib = nova_rt_to_int(b);
    if (ib == 0) return nova_rt_make_error("ZeroDivisionError: modulo by zero");
    return nova_rt_const_int(nova_rt_to_int(a) % ib);
}
NovaValue* nova_rt_pow(NovaValue* a, NovaValue* b) {
    if (!is_numeric(a) || !is_numeric(b)) return nova_rt_make_error("TypeError: unsupported operand types for ^");
    return nova_rt_const_float(std::pow(nova_rt_to_float(a), nova_rt_to_float(b)));
}

NovaValue* nova_rt_eq(NovaValue* a, NovaValue* b) { return nova_rt_const_bool(value_equal(a, b)); }
NovaValue* nova_rt_ne(NovaValue* a, NovaValue* b) { return nova_rt_const_bool(!value_equal(a, b)); }

static int compare_values(NovaValue* a, NovaValue* b, bool& ok) {
    ok = true;
    if (is_numeric(a) && is_numeric(b)) {
        double x = nova_rt_to_float(a), y = nova_rt_to_float(b);
        return (x < y) ? -1 : (x > y ? 1 : 0);
    }
    if (a && b && a->type == NOVA_STRING && b->type == NOVA_STRING) {
        int c = a->as.obj->str.compare(b->as.obj->str);
        return (c < 0) ? -1 : (c > 0 ? 1 : 0);
    }
    ok = false;
    return 0;
}
NovaValue* nova_rt_lt(NovaValue* a, NovaValue* b) { bool ok; int c = compare_values(a, b, ok); return nova_rt_const_bool(ok && c < 0); }
NovaValue* nova_rt_le(NovaValue* a, NovaValue* b) { bool ok; int c = compare_values(a, b, ok); return nova_rt_const_bool(ok && c <= 0); }
NovaValue* nova_rt_gt(NovaValue* a, NovaValue* b) { bool ok; int c = compare_values(a, b, ok); return nova_rt_const_bool(ok && c > 0); }
NovaValue* nova_rt_ge(NovaValue* a, NovaValue* b) { bool ok; int c = compare_values(a, b, ok); return nova_rt_const_bool(ok && c >= 0); }

NovaValue* nova_rt_and(NovaValue* a, NovaValue* b) { return nova_rt_const_bool(nova_rt_to_bool(a) && nova_rt_to_bool(b)); }
NovaValue* nova_rt_or(NovaValue* a, NovaValue* b)  { return nova_rt_const_bool(nova_rt_to_bool(a) || nova_rt_to_bool(b)); }
NovaValue* nova_rt_not(NovaValue* v)               { return nova_rt_const_bool(!nova_rt_to_bool(v)); }
NovaValue* nova_rt_neg(NovaValue* v) {
    if (v && v->type == NOVA_FLOAT) return nova_rt_const_float(-v->as.f);
    if (v && (v->type == NOVA_INT || v->type == NOVA_BOOL)) return nova_rt_const_int(-nova_rt_to_int(v));
    return nova_rt_make_error("TypeError: unsupported operand type for unary -");
}
NovaValue* nova_rt_select(NovaValue* cond, NovaValue* a, NovaValue* b) {
    return nova_rt_retain(nova_rt_to_bool(cond) ? a : b);
}

// ============================================================================
// Collections & Member Access
// ============================================================================
NovaValue* nova_rt_index(NovaValue* target, NovaValue* key) {
    if (!target) return nova_rt_const_null();
    if (target->type == NOVA_ARRAY || target->type == NOVA_TUPLE) {
        auto& a = target->as.obj->arr;
        int64_t idx = nova_rt_to_int(key);
        if (idx < 0) idx += (int64_t)a.size();
        if (idx < 0 || idx >= (int64_t)a.size()) return nova_rt_make_error("IndexError: index out of range");
        return nova_rt_retain(a[idx]);
    }
    if (target->type == NOVA_STRING) {
        auto& s = target->as.obj->str;
        int64_t idx = nova_rt_to_int(key);
        if (idx < 0) idx += (int64_t)s.size();
        if (idx < 0 || idx >= (int64_t)s.size()) return nova_rt_make_error("IndexError: string index out of range");
        return nova_rt_const_string(std::string(1, s[idx]).c_str());
    }
    if (target->type == NOVA_MAP) {
        for (auto& kv : target->as.obj->map) if (value_equal(kv.first, key)) return nova_rt_retain(kv.second);
        return nova_rt_const_null();
    }
    return nova_rt_const_null();
}

NovaValue* nova_rt_member(NovaValue* target, const char* name) {
    if (!target || !name) return nova_rt_const_null();
    std::string n = name;
    if (n == "length" || n == "size") {
        if (target->type == NOVA_ARRAY || target->type == NOVA_TUPLE) return nova_rt_const_int((int64_t)target->as.obj->arr.size());
        if (target->type == NOVA_MAP) return nova_rt_const_int((int64_t)target->as.obj->map.size());
        if (target->type == NOVA_STRING) return nova_rt_const_int((int64_t)target->as.obj->str.size());
    }
    if (target->type == NOVA_MAP) {
        NovaValue* key = nova_rt_const_string(name);
        NovaValue* result = nova_rt_const_null();
        for (auto& kv : target->as.obj->map) {
            if (value_equal(kv.first, key)) { nova_rt_release(result); result = nova_rt_retain(kv.second); break; }
        }
        nova_rt_release(key);
        return result;
    }
    return nova_rt_const_null();
}

NovaValue* nova_rt_slice(NovaValue* target, NovaValue* start, NovaValue* end, NovaValue* step) {
    if (!target) return nova_rt_const_null();
    int64_t st = (step && step->type != NOVA_NULL) ? nova_rt_to_int(step) : 1;
    if (st == 0) st = 1;
    auto normalize = [&](int64_t len) -> std::pair<int64_t, int64_t> {
        int64_t s = (start && start->type != NOVA_NULL) ? nova_rt_to_int(start) : (st > 0 ? 0 : len - 1);
        int64_t e = (end   && end->type   != NOVA_NULL) ? nova_rt_to_int(end)   : (st > 0 ? len : -1);
        if (s < 0) s += len;
        if (e < 0 && end && end->type != NOVA_NULL) e += len;
        return {s, e}; // NOTE: exotic negative-step + negative-end combos are a known follow-up
    };
    if (target->type == NOVA_STRING) {
        auto& s = target->as.obj->str;
        int64_t len = (int64_t)s.size();
        auto pr = normalize(len);
        std::string out;
        if (st > 0) for (int64_t i = pr.first; i < pr.second && i < len; i += st) { if (i >= 0) out += s[i]; }
        else        for (int64_t i = pr.first; i > pr.second && i >= 0; i += st)  { if (i < len) out += s[i]; }
        return nova_rt_const_string(out.c_str());
    }
    if (target->type == NOVA_ARRAY || target->type == NOVA_TUPLE) {
        auto& a = target->as.obj->arr;
        int64_t len = (int64_t)a.size();
        auto pr = normalize(len);
        NovaValue* r = new_object_value(target->type);
        if (st > 0) for (int64_t i = pr.first; i < pr.second && i < len; i += st) { if (i >= 0) r->as.obj->arr.push_back(nova_rt_retain(a[i])); }
        else        for (int64_t i = pr.first; i > pr.second && i >= 0; i += st)  { if (i < len) r->as.obj->arr.push_back(nova_rt_retain(a[i])); }
        return r;
    }
    return nova_rt_const_null();
}

NovaValue* nova_rt_make_array(NovaValue** args, int64_t count) {
    NovaValue* r = new_object_value(NOVA_ARRAY);
    for (int64_t i = 0; i < count; ++i) r->as.obj->arr.push_back(nova_rt_retain(args[i]));
    return r;
}
NovaValue* nova_rt_make_tuple(NovaValue** args, int64_t count) {
    NovaValue* r = new_object_value(NOVA_TUPLE);
    for (int64_t i = 0; i < count; ++i) r->as.obj->arr.push_back(nova_rt_retain(args[i]));
    return r;
}
NovaValue* nova_rt_make_map(NovaValue** args, int64_t pairCount) {
    NovaValue* r = new_object_value(NOVA_MAP);
    for (int64_t i = 0; i < pairCount; ++i) {
        r->as.obj->map.push_back({ nova_rt_retain(args[2 * i]), nova_rt_retain(args[2 * i + 1]) });
    }
    return r;
}

// ============================================================================
// Exceptions
// ============================================================================
NovaValue* nova_rt_make_error(const char* message) {
    NovaValue* e = new_object_value(NOVA_ERROR);
    e->as.obj->errMsg = message ? message : "error";
    return e;
}
bool nova_rt_error_check(NovaValue* v) { return v && v->type == NOVA_ERROR; }

// ============================================================================
// Standard Library Dispatcher
// ============================================================================
namespace {
struct NativeEntry { NovaValue* (*fn)(NovaValue**, int64_t); int arity; };
std::unordered_map<std::string, NativeEntry>& registry() {
    static std::unordered_map<std::string, NativeEntry> g;
    return g;
}
}

void nova_rt_register_native(const char* name, void* fnPtr, int arity) {
    if (!name || !fnPtr) return;
    registry()[name] = NativeEntry{ reinterpret_cast<NovaValue* (*)(NovaValue**, int64_t)>(fnPtr), arity };
}

NovaValue* nova_rt_call(const char* name, NovaValue** args, int64_t argc) {
    if (!name) return nova_rt_make_error("CallError: null function name");
    auto it = registry().find(name);
    if (it == registry().end()) return nova_rt_make_error(("CallError: unknown native function '" + std::string(name) + "'").c_str());
    if (it->second.arity >= 0 && (int64_t)it->second.arity != argc)
        return nova_rt_make_error(("ArityError: '" + std::string(name) + "' expected " + std::to_string(it->second.arity) + " args, got " + std::to_string(argc)).c_str());
    return it->second.fn(args, argc);
}

// ============================================================================
// Async & Generators (thread-based coroutine model — see header note)
// ============================================================================
NovaValue* nova_rt_make_generator(NovaGeneratorBody body, void* ctx) {
    NovaValue* g = new_object_value(NOVA_GENERATOR);
    NovaObject* o = g->as.obj;
    NovaValue* selfKeepAlive = nova_rt_retain(g); // released by the worker thread on completion
    o->gthread = std::thread([o, body, ctx, selfKeepAlive]() {
        {
            std::unique_lock<std::mutex> lk(o->gmu);
            o->gcv.wait(lk, [o] { return o->gWantRun || o->gDone; });
            o->gWantRun = false;
        }
        NovaValue* result = nullptr;
        if (!o->gDone) {
            t_currentGenerator = o;
            result = body(selfKeepAlive, ctx);
            t_currentGenerator = nullptr;
        }
        {
            std::lock_guard<std::mutex> lk(o->gmu);
            if (o->gPending) nova_rt_release(o->gPending);
            o->gPending = result;
            o->gHasValue = true;
            o->gDone = true;
        }
        o->gcv.notify_all();
        nova_rt_release(selfKeepAlive);
    });
    return g;
}

bool nova_rt_generator_done(NovaValue* g) {
    if (!g || g->type != NOVA_GENERATOR) return true;
    std::lock_guard<std::mutex> lk(g->as.obj->gmu);
    return g->as.obj->gDone;
}

NovaValue* nova_rt_yield(NovaValue* v) {
    NovaObject* o = t_currentGenerator;
    if (!o) return v; // called outside a generator thread: no-op passthrough
    std::unique_lock<std::mutex> lk(o->gmu);
    if (o->gPending) nova_rt_release(o->gPending);
    o->gPending = v;
    o->gHasValue = true;
    o->gcv.notify_all();
    o->gcv.wait(lk, [o] { return o->gWantRun || o->gDone; });
    o->gWantRun = false;
    return nova_rt_const_null(); // V1: no two-way .send() payload yet
}

void nova_rt_async_resume(NovaValue* v) {
    if (!v || v->type != NOVA_GENERATOR) return;
    NovaObject* o = v->as.obj;
    std::unique_lock<std::mutex> lk(o->gmu);
    if (o->gDone) return;
    o->gHasValue = false;
    o->gWantRun = true;
    o->gcv.notify_all();
    o->gcv.wait(lk, [o] { return o->gHasValue || o->gDone; });
}

void nova_rt_async_suspend(NovaValue* v) {
    if (!v || v->type != NOVA_GENERATOR) return;
    NovaObject* o = v->as.obj;
    std::unique_lock<std::mutex> lk(o->gmu);
    o->gcv.wait(lk, [o] { return o->gHasValue || o->gDone; });
}

NovaValue* nova_rt_await(NovaValue* awaitable) {
    if (!awaitable) return nova_rt_const_null();
    if (awaitable->type == NOVA_GENERATOR) {
        NovaObject* o = awaitable->as.obj;
        bool ready;
        { std::lock_guard<std::mutex> lk(o->gmu); ready = o->gHasValue || o->gDone; }
        if (!ready) nova_rt_async_resume(awaitable);
        std::lock_guard<std::mutex> lk(o->gmu);
        return o->gPending ? nova_rt_retain(o->gPending) : nova_rt_const_null();
    }
    return nova_rt_retain(awaitable); // plain value: already "resolved"
}

// ============================================================================
// Runtime Lifecycle & Entry Point
// ============================================================================
extern "C" void nova_fn___nova_main(); // provided by the compiled Nova program

static bool g_initialized = false;

void nova_rt_init(void) {
    if (g_initialized) return;
    g_initialized = true;
}

void nova_rt_concurrency_shutdown(void) {
    // Live generator threads are joined individually as their NovaValue is
    // released (see release_object). Nothing global to flush yet — this
    // hook exists for a future shared thread-pool/scheduler.
}

void nova_rt_shutdown(void) {
    nova_rt_concurrency_shutdown();
    g_initialized = false;
}

int nova_main(void) {
    nova_rt_init();
    nova_fn___nova_main();
    nova_rt_shutdown();
    return 0;
}
