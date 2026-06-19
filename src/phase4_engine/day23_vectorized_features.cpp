#include <iostream>
#include <vector>
#include <chrono>
#include <immintrin.h>
#include <random>
#include <iomanip>
#include <cstdint>

constexpr int SYMS = 4;          // Native AVX2 width (4x double)
constexpr int TICKS = 50'000'000;

// 🔹 SoA Layout for SIMD Cache Efficiency
struct alignas(32) SimdPipeline {
    // Input streams
    double prices[SYMS]  __attribute__((aligned(32)));
    double volumes[SYMS] __attribute__((aligned(32)));
    
    // State vectors
    double ema_fast[SYMS], ema_slow[SYMS];
    double alpha_fast[SYMS], alpha_slow[SYMS];
    double mean[SYMS], m2[SYMS], n[SYMS]; // Welford's
    double std_dev[SYMS], z_score[SYMS];
};

// 🔹 Initialize with safe defaults
void init_pipeline(SimdPipeline& p, double init_price) {
    for (int i = 0; i < SYMS; ++i) {
        p.ema_fast[i] = p.ema_slow[i] = init_price;
        p.mean[i] = init_price; p.m2[i] = 0.0; p.n[i] = 1.0;
        p.alpha_fast[i] = 2.0 / (11.0); // 10-tick EMA
        p.alpha_slow[i] = 2.0 / (51.0); // 50-tick EMA
    }
}

// ✅ SIMD UPDATE (AVX2)
void update_simd(SimdPipeline& p) {
    // Load 4 symbols into registers
    __m256d v_price = _mm256_load_pd(p.prices);
    __m256d v_vol   = _mm256_load_pd(p.volumes);
    
    __m256d v_ef = _mm256_load_pd(p.ema_fast);
    __m256d v_es = _mm256_load_pd(p.ema_slow);
    __m256d v_af = _mm256_load_pd(p.alpha_fast);
    __m256d v_as = _mm256_load_pd(p.alpha_slow);
    __m256d v_m  = _mm256_load_pd(p.mean);
    __m256d v_m2 = _mm256_load_pd(p.m2);
    __m256d v_n  = _mm256_load_pd(p.n);

    // 1️⃣ EMA Updates (vectorized)
    __m256d d_f = _mm256_sub_pd(v_price, v_ef);
    v_ef = _mm256_add_pd(v_ef, _mm256_mul_pd(v_af, d_f));
    
    __m256d d_s = _mm256_sub_pd(v_price, v_es);
    v_es = _mm256_add_pd(v_es, _mm256_mul_pd(v_as, d_s));

    // 2️⃣ Welford's Online Variance (vectorized)
    __m256d v_n1 = _mm256_add_pd(v_n, _mm256_set1_pd(1.0));
    __m256d delta = _mm256_sub_pd(v_price, v_m);
    v_m = _mm256_add_pd(v_m, _mm256_div_pd(delta, v_n1)); // Division lane
    __m256d delta2 = _mm256_sub_pd(v_price, v_m);
    v_m2 = _mm256_add_pd(v_m2, _mm256_mul_pd(delta, delta2));
    v_n = v_n1;

    // 3️⃣ Compute StdDev & Z-Score
    __m256d v_var = _mm256_div_pd(v_m2, _mm256_sub_pd(v_n, _mm256_set1_pd(1.0)));
    // Approx sqrt via _mm256_sqrt_pd (accurate, ~5-7 cycles)
    __m256d v_std = _mm256_sqrt_pd(v_var);
    __m256d v_z   = _mm256_div_pd(_mm256_sub_pd(v_price, v_m), v_std);

    // Store results back
    _mm256_store_pd(p.ema_fast, v_ef);
    _mm256_store_pd(p.ema_slow, v_es);
    _mm256_store_pd(p.mean, v_m);
    _mm256_store_pd(p.m2, v_m2);
    _mm256_store_pd(p.n, v_n);
    _mm256_store_pd(p.std_dev, v_std);
    _mm256_store_pd(p.z_score, v_z);
}

// ❌ SCALAR BASELINE (Identical logic, loop unrolled manually by compiler)
void update_scalar(SimdPipeline& p) {
    for (int i = 0; i < SYMS; ++i) {
        double price = p.prices[i];
        
        // EMA
        p.ema_fast[i] += p.alpha_fast[i] * (price - p.ema_fast[i]);
        p.ema_slow[i] += p.alpha_slow[i] * (price - p.ema_slow[i]);
        
        // Welford
        double n1 = p.n[i] + 1.0;
        double delta = price - p.mean[i];
        p.mean[i] += delta / n1;
        double delta2 = price - p.mean[i];
        p.m2[i] += delta * delta2;
        p.n[i] = n1;
        
        // Std/Z
        double var = p.m2[i] / (p.n[i] - 1.0);
        p.std_dev[i] = std::sqrt(var);
        p.z_score[i] = (price - p.mean[i]) / p.std_dev[i];
    }
}

int main() {
    SimdPipeline sim;
    init_pipeline(sim, 150.0);
    
    std::mt19937_64 rng(42);
    std::normal_distribution<double> dist(0.0, 0.02);

    std::cout << "=== SIMD Multi-Symbol Pipeline Benchmark ===\n";
    std::cout << "Symbols: " << SYMS << " | Ticks: " << TICKS << "\n\n";

    // Warmup CPU & branch predictor
    update_simd(sim); update_scalar(sim);

    // 🚀 SIMD Benchmark
    auto start_s = std::chrono::high_resolution_clock::now();
    for (int t = 0; t < TICKS; ++t) {
        for (int i = 0; i < SYMS; ++i) {
            sim.prices[i] += dist(rng);
            sim.volumes[i] = 1000.0 + rng() % 5000;
        }
        update_simd(sim);
    }
    auto end_s = std::chrono::high_resolution_clock::now();
    double ms_simd = std::chrono::duration_cast<std::chrono::milliseconds>(end_s - start_s).count();

    // 🔁 Scalar Benchmark
    auto start_c = std::chrono::high_resolution_clock::now();
    for (int t = 0; t < TICKS; ++t) {
        for (int i = 0; i < SYMS; ++i) {
            sim.prices[i] += dist(rng);
            sim.volumes[i] = 1000.0 + rng() % 5000;
        }
        update_scalar(sim);
    }
    auto end_c = std::chrono::high_resolution_clock::now();
    double ms_scalar = std::chrono::duration_cast<std::chrono::milliseconds>(end_c - start_c).count();

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Scalar   : " << ms_scalar << " ms\n";
    std::cout << "AVX2 SIMD: " << ms_simd  << " ms\n";
    std::cout << "Speedup  : " << (ms_scalar / std::max(0.1, ms_simd)) << "x\n\n";

    std::cout << "📊 SIMD Engineering Rules:\n";
    std::cout << " • SoA > AoS: Contiguous arrays enable `_mm256_load_pd` without scatter/gather.\n";
    std::cout << " • Division is the bottleneck: `_mm256_div_pd` takes ~15 cycles. Production uses reciprocal tables.\n";
    std::cout << " • Loop unrolling isn't enough: SIMD processes 4 lanes per instruction, reducing dispatch overhead.\n";
    std::cout << " • Production: AVX-512 `_mm512_mask_compressstoreu_pd` + ZMM registers for 8-16 symbols.\n";

    return 0;
}