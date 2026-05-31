#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#include <array>
#include <string_view>

// ❌ RUNTIME VERSION: Division, branching, dynamic checks per tick
double price_adjust_runtime(double price, double vol_buffer, double tick_size, double max_cap) {
    double raw = price * (1.0 + vol_buffer);
    double adjusted = std::round(raw / tick_size) * tick_size;
    if (adjusted > max_cap) return max_cap; // Runtime branch
    return adjusted;
}

// ✅ COMPILE-TIME VERSION: Constants hoisted, reciprocal precomputed, branch hints
constexpr double VOL_BUFFER = 0.05;               // 5% volatility buffer
constexpr double TICK_SIZE  = 0.01;               // Minimum tick
constexpr double MAX_CAP    = 1000.0;             // Risk limit
constexpr double TICK_INV   = 1.0 / TICK_SIZE;    // Precompute reciprocal at compile time

// constexpr enables compile-time evaluation + aggressive runtime inlining
constexpr double price_adjust_constexpr(double price) {
    double raw = price * (1.0 + VOL_BUFFER);
    // Multiplication by reciprocal is faster than division on x86/ARM
    double adjusted = std::round(raw * TICK_INV) * TICK_SIZE;
    // Compiler evaluates MAX_CAP at compile time → enables branch prediction hints
    return (adjusted > MAX_CAP) ? MAX_CAP : adjusted;
}

// 🛡️ COMPILE-TIME CONFIG VALIDATION (C++17/20)
// Catches misconfigurations before deployment, not during market hours
// Use constexpr for wider compiler compatibility (constexpr is evaluated at compile-time when possible)
constexpr bool validate_strategy_params(double min_spread, double max_spread) {
    return min_spread >= 0.0 && max_spread > min_spread && max_spread < 10.0;
}

static_assert(validate_strategy_params(0.01, 2.0), 
    "⚠️  Strategy config invalid! Check min/max spread parameters.");

void benchmark() {
    constexpr int N = 10'000'000;
    std::vector<double> prices(N);
    for (int i = 0; i < N; ++i) prices[i] = 150.0 + (i % 200) * 0.01;

    // Runtime
    auto start_rt = std::chrono::high_resolution_clock::now();
    volatile double rt_sum = 0;
    for (double p : prices) {
        rt_sum += price_adjust_runtime(p, VOL_BUFFER, TICK_SIZE, MAX_CAP);
    }
    auto end_rt = std::chrono::high_resolution_clock::now();
    auto ms_rt = std::chrono::duration_cast<std::chrono::milliseconds>(end_rt - start_rt).count();

    // Constexpr-optimized (inlined + reciprocal + branch elimination)
    auto start_ct = std::chrono::high_resolution_clock::now();
    volatile double ct_sum = 0;
    for (double p : prices) {
        ct_sum += price_adjust_constexpr(p);
    }
    auto end_ct = std::chrono::high_resolution_clock::now();
    auto ms_ct = std::chrono::duration_cast<std::chrono::milliseconds>(end_ct - start_ct).count();

    std::cout << "=== constexpr vs Runtime Benchmark ===\n";
    std::cout << "Runtime:   " << ms_rt << " ms\n";
    std::cout << "Constexpr: " << ms_ct << " ms\n";
    std::cout << "Speedup:   " << (double)ms_rt / (ms_ct == 0 ? 0.1 : ms_ct) << "x\n";
}

int main() {
    benchmark();
    return 0;
}