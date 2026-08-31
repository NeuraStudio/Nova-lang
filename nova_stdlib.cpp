#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>
#include <initializer_list>

extern "C" {
    typedef struct NovaValue NovaValue;
    NovaValue* nova_rt_call(const char* name, NovaValue** args, int64_t argc);
    NovaValue* nova_rt_const_int(int64_t v);
    NovaValue* nova_rt_const_float(double v);
    NovaValue* nova_rt_const_string(const char* v);
    NovaValue* nova_rt_const_bool(bool v);
    NovaValue* nova_rt_const_null(void);
    int64_t nova_rt_to_int(NovaValue* v);
    double nova_rt_to_float(NovaValue* v);
    bool nova_rt_to_bool(NovaValue* v);

    NovaValue* nova_std_math_sin(NovaValue* value) {
        if (!value) return nova_rt_const_null();
        return nova_rt_const_float(std::sin(nova_rt_to_float(value)));
    }
    NovaValue* nova_std_math_cos(NovaValue* value) {
        if (!value) return nova_rt_const_null();
        return nova_rt_const_float(std::cos(nova_rt_to_float(value)));
    }
    NovaValue* nova_std_math_sqrt(NovaValue* value) {
        if (!value) return nova_rt_const_null();
        const double x = nova_rt_to_float(value);
        if (x < 0.0) return nova_rt_const_null();
        return nova_rt_const_float(std::sqrt(x));
    }
    NovaValue* nova_std_time_now(void) {
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        return nova_rt_const_int(static_cast<int64_t>(ms));
    }
    NovaValue* nova_std_math_random(void) {
        thread_local std::mt19937_64 engine([] {
            std::random_device rd;
            return std::mt19937_64(rd());
        }());
        std::uniform_real_distribution<double> distribution(0.0, 1.0);
        return nova_rt_const_float(distribution(engine));
    }
}
