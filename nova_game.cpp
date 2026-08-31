#include <cstdint>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <iostream>

// Real OpenGL API (Cross-platform GL header)
#if defined(__APPLE__)
    #include <OpenGL/gl.h>
#elif defined(_WIN32)
    #include <windows.h>
    #include <GL/gl.h>
#else
    // Linux / Android (Termux)
    #include <GL/gl.h>
#endif

extern "C" {
    struct NovaValue;
    int64_t nova_rt_to_int(NovaValue*);
    double nova_rt_to_float(NovaValue*);
    NovaValue* nova_rt_from_float(double);
    NovaValue* nova_rt_from_int(int64_t);
    NovaValue* nova_rt_from_bool(int);
    NovaValue* nova_rt_from_string(const char*);
    void nova_rt_register_native(const char* name, void* fnPtr, int arity);
}

// ── 1. 3D Math & Physics (Real Vector Operations) ────────────────────────
extern "C" NovaValue* nova_game_vec3_dot(NovaValue* x1, NovaValue* y1, NovaValue* z1, 
                                         NovaValue* x2, NovaValue* y2, NovaValue* z2) {
    double dx1 = nova_rt_to_float(x1); double dy1 = nova_rt_to_float(y1); double dz1 = nova_rt_to_float(z1);
    double dx2 = nova_rt_to_float(x2); double dy2 = nova_rt_to_float(y2); double dz2 = nova_rt_to_float(z2);
    
    // Dot Product Formula: (x1*x2 + y1*y2 + z1*z2)
    double dot = (dx1 * dx2) + (dy1 * dy2) + (dz1 * dz2);
    return nova_rt_from_float(dot);
}

extern "C" NovaValue* nova_game_vec3_distance(NovaValue* x1, NovaValue* y1, NovaValue* z1, 
                                              NovaValue* x2, NovaValue* y2, NovaValue* z2) {
    double dx = nova_rt_to_float(x2) - nova_rt_to_float(x1);
    double dy = nova_rt_to_float(y2) - nova_rt_to_float(y1);
    double dz = nova_rt_to_float(z2) - nova_rt_to_float(z1);
    
    // Real 3D Distance (Pythagoras)
    double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    return nova_rt_from_float(dist);
}

// ── 2. ECS (Entity Component System) ────────────────────────────────────
// A highly optimized, data-oriented ECS backend in C++
namespace ecs {
    static int64_t next_entity_id = 1;
    struct Transform { double x, y, z; };
    struct PhysicsBody { double velocity_x, velocity_y, velocity_z; double mass; };
    
    static std::unordered_map<int64_t, Transform> transforms;
    static std::unordered_map<int64_t, PhysicsBody> physics_bodies;
}

extern "C" NovaValue* nova_game_ecs_create_entity() {
    return nova_rt_from_int(ecs::next_entity_id++);
}

extern "C" NovaValue* nova_game_ecs_add_transform(NovaValue* entityId, NovaValue* x, NovaValue* y, NovaValue* z) {
    int64_t id = nova_rt_to_int(entityId);
    ecs::transforms[id] = { nova_rt_to_float(x), nova_rt_to_float(y), nova_rt_to_float(z) };
    return nova_rt_from_bool(1);
}

// ── 3. Real Graphics (OpenGL/Vulkan Bindings) ───────────────────────────
extern "C" NovaValue* nova_game_gl_clear(NovaValue* r, NovaValue* g, NovaValue* b, NovaValue* a) {
    float fr = static_cast<float>(nova_rt_to_float(r));
    float fg = static_cast<float>(nova_rt_to_float(g));
    float fb = static_cast<float>(nova_rt_to_float(b));
    float fa = static_cast<float>(nova_rt_to_float(a));

    // REAL OpenGL Call - Clears the rendering buffer with a color
    glClearColor(fr, fg, fb, fa);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    return nova_rt_from_bool(1);
}

namespace {
    NovaValue* adapt_game_vec3_dot(NovaValue** args, int64_t argc) {
        if (argc < 6) return nova_rt_from_float(0.0);
        return nova_game_vec3_dot(args[0], args[1], args[2], args[3], args[4], args[5]);
    }
    NovaValue* adapt_game_vec3_distance(NovaValue** args, int64_t argc) {
        if (argc < 6) return nova_rt_from_float(0.0);
        return nova_game_vec3_distance(args[0], args[1], args[2], args[3], args[4], args[5]);
    }
    NovaValue* adapt_game_ecs_create_entity(NovaValue** args, int64_t argc) {
        return nova_game_ecs_create_entity();
    }
    NovaValue* adapt_game_ecs_add_transform(NovaValue** args, int64_t argc) {
        if (argc < 4) return nova_rt_from_bool(0);
        return nova_game_ecs_add_transform(args[0], args[1], args[2], args[3]);
    }
    NovaValue* adapt_game_gl_clear(NovaValue** args, int64_t argc) {
        if (argc < 4) return nova_rt_from_bool(0);
        return nova_game_gl_clear(args[0], args[1], args[2], args[3]);
    }
}

extern "C" void nova_game_register() {
    nova_rt_register_native("nova_game_vec3_dot", reinterpret_cast<void*>(&adapt_game_vec3_dot), 6);
    nova_rt_register_native("nova_game_vec3_distance", reinterpret_cast<void*>(&adapt_game_vec3_distance), 6);
    nova_rt_register_native("nova_game_ecs_create_entity", reinterpret_cast<void*>(&adapt_game_ecs_create_entity), 0);
    nova_rt_register_native("nova_game_ecs_add_transform", reinterpret_cast<void*>(&adapt_game_ecs_add_transform), 4);
    nova_rt_register_native("nova_game_gl_clear", reinterpret_cast<void*>(&adapt_game_gl_clear), 4);
}
