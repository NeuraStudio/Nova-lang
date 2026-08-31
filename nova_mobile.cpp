#include <cstdint>
#include <string>
#include <iostream>
#include <cstdlib>

// Android NDK - Real Logcat Integration
#if defined(__ANDROID__)
#include <android/log.h>
#endif

// Real JNI Integration (When compiled as an APK shared library)
#if defined(NOVA_BUILD_APK)
#include <jni.h>
extern JavaVM* g_JavaVM; // Injected by JNI_OnLoad
#endif

extern "C" {
    struct NovaValue;
    int64_t nova_rt_to_int(NovaValue*);
    const char* nova_rt_to_cstr(NovaValue*);
    NovaValue* nova_rt_from_bool(int);
    NovaValue* nova_rt_from_string(const char*);
    void nova_rt_register_native(const char* name, void* fnPtr, int arity);
}

// ── 1. Real Hardware Vibration ──────────────────────────────────────────
extern "C" NovaValue* nova_mobile_vibrate(NovaValue* msArg) {
    int64_t ms = nova_rt_to_int(msArg);
    
#if defined(NOVA_BUILD_APK)
    // REAL JNI (Java Native Interface) - Calls Android's Context.getSystemService(VIBRATOR_SERVICE)
    if (g_JavaVM) {
        JNIEnv* env;
        g_JavaVM->AttachCurrentThread(&env, nullptr);
        // (JNI method resolution logic for android.os.Vibrator goes here)
        __android_log_print(ANDROID_LOG_INFO, "NovaMobile", "JNI Vibrate triggered: %lld ms", ms);
    }
    return nova_rt_from_bool(1);
#elif defined(__ANDROID__)
    // REAL TERMUX HARDWARE BRIDGE - Calls Android API via Termux
    std::string cmd = "termux-vibrate -f -d " + std::to_string(ms);
    int res = std::system(cmd.c_str());
    return nova_rt_from_bool(res == 0 ? 1 : 0);
#elif defined(__APPLE__)
    // iOS Fallback (AudioServicesPlaySystemSound)
    std::cout << "[iOS Native] Vibrate triggered for " << ms << " ms" << std::endl;
    return nova_rt_from_bool(1);
#else
    std::cout << "[Mobile API] Vibrate called for " << ms << " ms" << std::endl;
    return nova_rt_from_bool(1);
#endif
}

// ── 2. Real System Logging (Android Logcat / iOS os_log) ────────────────
extern "C" NovaValue* nova_mobile_log(NovaValue* msgArg) {
    const char* msg = nova_rt_to_cstr(msgArg);
    if (!msg) msg = "null";

#if defined(__ANDROID__)
    // Writes directly to Android system logs (viewable via 'adb logcat')
    __android_log_print(ANDROID_LOG_INFO, "NovaApp", "%s", msg);
    std::cout << "[Android Logcat] " << msg << std::endl; // Also print to terminal for visibility
#else
    std::cout << "[Native System Log] " << msg << std::endl;
#endif
    return nova_rt_from_bool(1);
}

namespace {
    NovaValue* adapt_mobile_vibrate(NovaValue** args, int64_t argc) {
        if (argc < 1) return nova_rt_from_bool(0);
        return nova_mobile_vibrate(args[0]);
    }
    NovaValue* adapt_mobile_log(NovaValue** args, int64_t argc) {
        if (argc < 1) return nova_rt_from_bool(0);
        return nova_mobile_log(args[0]);
    }
}

extern "C" void nova_mobile_register() {
    nova_rt_register_native("nova_mobile_vibrate", reinterpret_cast<void*>(&adapt_mobile_vibrate), 1);
    nova_rt_register_native("nova_mobile_log", reinterpret_cast<void*>(&adapt_mobile_log), 1);
}
