// nova_rt.hpp — Nova Language Core Runtime Environment (canonical ABI)
// Single source of truth for NovaValue. nova_bridge.cpp and every codegen
// backend must include this header and never redefine the struct.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NovaType {
    NOVA_NULL      = 0,
    NOVA_INT       = 1,
    NOVA_FLOAT     = 2,
    NOVA_BOOL      = 3,
    NOVA_STRING    = 4,
    NOVA_ARRAY     = 5,
    NOVA_MAP       = 6,
    NOVA_TUPLE     = 7,
    NOVA_ERROR     = 8,
    NOVA_REF       = 9,   /* mutable memory cell — unsafe mode / var capture */
    NOVA_FUTURE    = 10,  /* async awaitable */
    NOVA_GENERATOR = 11   /* yield-based coroutine handle */
} NovaType;

/* Heap payload for non-scalar kinds. Internals defined only in nova_rt.cpp
   so the ABI struct below stays fixed-size and stable across versions. */
typedef struct NovaObject NovaObject;

typedef struct NovaValue {
    NovaType type;
    int32_t  refcount;
    union {
        int64_t     i;
        double      f;
        bool        b;
        NovaObject* obj;
    } as;
} NovaValue;

/* ============================================================================
 * Reference Counting
 * ========================================================================== */
NovaValue* nova_rt_retain(NovaValue* v);
void       nova_rt_release(NovaValue* v);

/* ============================================================================
 * Memory / Ref Cells (unsafe-mode & captured-variable boxes)
 * ========================================================================== */
NovaValue* nova_rt_alloc(int64_t size);
NovaValue* nova_rt_load(NovaValue* addr);
void       nova_rt_store(NovaValue* addr, NovaValue* value);

/* ============================================================================
 * Constant Boxing
 * ========================================================================== */
NovaValue* nova_rt_const_int(int64_t v);
NovaValue* nova_rt_const_float(double v);
NovaValue* nova_rt_const_string(const char* v);
NovaValue* nova_rt_const_bool(bool v);
NovaValue* nova_rt_const_null(void);

/* ============================================================================
 * Native Coercion & Unboxing
 * ========================================================================== */
int64_t    nova_rt_to_int(NovaValue* v);
double     nova_rt_to_float(NovaValue* v);
bool       nova_rt_to_bool(NovaValue* v);
const char* nova_rt_to_cstr(NovaValue* v);
NovaValue* nova_rt_to_string(NovaValue* v); /* display repr, any type -> NOVA_STRING */

NovaValue* nova_rt_from_int(int64_t v);
NovaValue* nova_rt_from_float(double v);
NovaValue* nova_rt_from_bool(bool v);
NovaValue* nova_rt_from_string(const char* v);

/* ============================================================================
 * Math & Logic Operations (Dynamically Typed)
 * ========================================================================== */
NovaValue* nova_rt_add(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_sub(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_mul(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_div(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_mod(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_pow(NovaValue* a, NovaValue* b);

NovaValue* nova_rt_eq(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_ne(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_lt(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_le(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_gt(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_ge(NovaValue* a, NovaValue* b);

NovaValue* nova_rt_and(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_or(NovaValue* a, NovaValue* b);
NovaValue* nova_rt_neg(NovaValue* v);
NovaValue* nova_rt_not(NovaValue* v);
NovaValue* nova_rt_select(NovaValue* cond, NovaValue* a, NovaValue* b);

/* ============================================================================
 * Collections & Member Access
 * ========================================================================== */
NovaValue* nova_rt_cast(NovaValue* v, const char* typeName);
NovaValue* nova_rt_index(NovaValue* target, NovaValue* key);
NovaValue* nova_rt_member(NovaValue* target, const char* name);
NovaValue* nova_rt_slice(NovaValue* target, NovaValue* start, NovaValue* end, NovaValue* step);

NovaValue* nova_rt_make_array(NovaValue** args, int64_t count);
NovaValue* nova_rt_make_map(NovaValue** args, int64_t pairCount); /* args = [k0,v0,k1,v1,...] */
NovaValue* nova_rt_make_tuple(NovaValue** args, int64_t count);

/* ============================================================================
 * Exceptions & Error Handling
 * ========================================================================== */
NovaValue* nova_rt_make_error(const char* message);
bool       nova_rt_error_check(NovaValue* v);

/* ============================================================================
 * Standard Library Dispatcher
 * ========================================================================== */
/* arity: pass -1 to skip argc validation. */
void       nova_rt_register_native(const char* name, void* fnPtr, int arity);
NovaValue* nova_rt_call(const char* name, NovaValue** args, int64_t argc);

/* ============================================================================
 * Async & Generator Hooks
 * V1 model: each generator runs its body on its own worker thread and
 * blocks between yields on a condvar. Correct and simple; trades a thread
 * stack per live generator for not needing stackful-fiber support on
 * constrained hardware. Revisit with ucontext/fiber-based coroutines if
 * generator-heavy workloads become a memory bottleneck on-device.
 * ========================================================================== */
typedef NovaValue* (*NovaGeneratorBody)(NovaValue* self, void* ctx);
NovaValue* nova_rt_make_generator(NovaGeneratorBody body, void* ctx);
bool       nova_rt_generator_done(NovaValue* g);

NovaValue* nova_rt_await(NovaValue* awaitable);
NovaValue* nova_rt_yield(NovaValue* v);        /* call from inside a generator body */
void       nova_rt_async_suspend(NovaValue* v); /* park caller until a value is ready */
void       nova_rt_async_resume(NovaValue* v);  /* drive generator to its next yield */

/* ============================================================================
 * Runtime Lifecycle & Entry Point
 * ========================================================================== */
void       nova_rt_init(void);
void       nova_rt_concurrency_shutdown(void);
void       nova_rt_shutdown(void);

/* The ONE canonical entry point: inits the runtime, calls the codegen'd
   nova_fn___nova_main(), shuts down. main() in nova_bridge.cpp calls this
   — do not duplicate this logic anywhere else. */
int        nova_main(void);

#ifdef __cplusplus
}
#endif
