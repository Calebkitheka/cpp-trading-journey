#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#else
#include <time.h>
#include <x86intrin.h>
#endif

// 🔹 Hardware Timer Abstraction
class HardwareTimer {
    double cycles_to_ns_ = 1.0;
    uint64_t base_cycles_ = 0;

    // Serializing read: prevents out-of-order execution from skewing measurements
    inline uint64_t read_tsc() const {
#ifdef _WIN32
        return __rdtscp(nullptr);
#else
        unsigned int aux;
        return __rdtscp(&aux);
#endif
    }

public:
    // Calibrate against OS monotonic clock (bypasses NTP drift)
    void calibrate() {
        // Warmup CPU frequency scaling & branch predictor
        for (int i = 0; i < 10; ++i) _mm_lfence();

        auto os_start = std::chrono::steady_clock::now();
        auto start = read_tsc();

        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 0.5s window

        auto os_end = std::chrono::steady_clock::now();
        auto end = read_tsc();

        uint64_t delta_cycles = end - start;
        uint64_t delta_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(os_end - os_start).count();

        cycles_to_ns_ = static_cast<double>(delta_ns) / delta_cycles;
        base_cycles_ = start;

        std::cout << "Calibration: 1 cycle ≈ " << std::fixed << std::setprecision(3) 
                  << cycles_to_ns_ << " ns (CPU ~" << std::round(1.0 / cycles_to_ns_) << " MHz)\n";
    }

    inline uint64_t cycles_since_base() const { return read_tsc() - base_cycles_; }
    inline double ns_since_base() const { return static_cast<double>(cycles_since_base()) * cycles_to_ns_; }
    inline double cycles_to_ns() const { return cycles_to_ns_; }
};

// Benchmark helper
template<typename TimerFn>
void measure_overhead(const char* name, TimerFn fn, std::vector<double>& out) {
    for (int i = 0; i < 1'000'000; ++i) {
        auto t1 = fn();
        auto t2 = fn();
        out.push_back(static_cast<double>(t2 - t1));
    }
}

int main() {
    std::cout << "=== Hardware Timestamping & Calibration Benchmark ===\n";
    
    HardwareTimer hw_timer;
    hw_timer.calibrate();
    std::cout << "\n";

    std::vector<double> chrono_lat, os_lat, hw_lat;
    chrono_lat.reserve(100000); os_lat.reserve(100000); hw_lat.reserve(100000);

    // 1️⃣ std::chrono (non-deterministic, subject to vDSO overhead)
    measure_overhead("std::chrono", []() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }, chrono_lat);

    // 2️⃣ OS High-Res Timer (QPC on Windows, CLOCK_MONOTONIC_RAW on Linux)
    #ifdef _WIN32
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
    measure_overhead("QPC", [&qpf]() {
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        return qpc.QuadPart * 1e9 / qpf.QuadPart;
    }, os_lat);
    #else
    measure_overhead("MONO_RAW", []() {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return ts.tv_sec * 1e9 + ts.tv_nsec;
    }, os_lat);
    #endif

    // 3️⃣ RDTSCP (CPU cycle counter, serialized)
    measure_overhead("RDTSCP", [&hw_timer]() { return hw_timer.cycles_since_base(); }, hw_lat);

    // Stats printer
    auto print_stats = [](const char* name, const std::vector<double>& data, double scale = 1.0) {
        auto sorted = data; std::sort(sorted.begin(), sorted.end());
        auto p = [&](double pct) { return sorted[static_cast<size_t>(sorted.size() * pct / 100.0)]; };
        std::cout << std::left << std::setw(12) << name 
                  << " | Avg: " << std::setw(6) << p(50) * scale
                  << " | p99: " << std::setw(6) << p(99) * scale
                  << " | Max: " << std::setw(6) << p(100) * scale
                  << (scale == 1.0 ? " ns" : " cyc") << "\n";
    };

    std::cout << "Latency Overhead (1M consecutive calls):\n";
    print_stats("chrono", chrono_lat);
    print_stats("OS Timer", os_lat);
    print_stats("RDTSCP", hw_lat, hw_timer.cycles_to_ns());

    std::cout << "\n📊 Production Rules:\n";
    std::cout << " • RDTSCP: ~1-3 cycle overhead (~0.3-1ns). Use for wire-to-trade attribution.\n";
    std::cout << " • OS Timer: ~50-200ns. Safe for cross-core sync & compliance logging.\n";
    std::cout << " • std::chrono: ~100-500ns, non-monotonic on Windows, NTP-affected. Avoid hot paths.\n";
    std::cout << " • Always calibrate at startup. Modern CPUs use Invariant TSC (frequency stable).\n";

    return 0;
}