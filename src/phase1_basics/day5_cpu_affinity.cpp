#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#include <pthread.h>
#endif

// Simulates CPU-heavy order matching / risk validation
void cpu_work() {
    volatile double result = 0;
    for (int i = 0; i < 50'000'000; ++i) {
        result += std::sqrt(i * 3.14159);
    }
}

// Platform-specific thread pinning
bool pin_to_core(int core_id) {
#ifdef _WIN32
    return SetThreadAffinityMask(GetCurrentThread(), 1ULL << core_id) != 0;
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
#endif
}

// Measure latency distribution across iterations
void measure_jitter(const std::string& name, int core_id, int iterations, std::vector<double>& out) {
    std::thread t([&]() {
        if (core_id >= 0) pin_to_core(core_id);

        for (int i = 0; i < iterations; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            cpu_work();
            auto end = std::chrono::high_resolution_clock::now();
            out.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        }
    });
    t.join();
}

void print_stats(const std::string& name, const std::vector<double>& latencies) {
    double min = *std::min_element(latencies.begin(), latencies.end());
    double max = *std::max_element(latencies.begin(), latencies.end());
    double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    double variance = 0;
    for (double l : latencies) variance += (l - avg) * (l - avg);
    double std_dev = std::sqrt(variance / latencies.size());

    std::cout << std::left << std::setw(25) << name
              << " | Min: " << std::setw(6) << min << " μs"
              << " | Max: " << std::setw(6) << max << " μs"
              << " | Avg: " << std::setw(6) << avg << " μs"
              << " | σ: " << std::setw(6) << std_dev << " μs\n";
}

int main() {
    constexpr int ITERATIONS = 30;
    std::cout << "=== CPU Affinity & Thread Jitter Benchmark ===\n";
    std::cout << "Iterations: " << ITERATIONS << " (measuring latency distribution)\n";
    std::cout << "⚠️  Tip: Choose a core NOT handling OS interrupts (avoid Core 0)\n\n";

    // Unpinned: OS scheduler migrates thread freely
    std::vector<double> unpinned_lat;
    measure_jitter("Unpinned (Default)", -1, ITERATIONS, unpinned_lat);
    print_stats("Unpinned", unpinned_lat);

    // Pinned: Thread locked to Core 2 (adjust for your CPU)
    std::vector<double> pinned_lat;
    measure_jitter("Pinned to Core 2", 2, ITERATIONS, pinned_lat);
    print_stats("Pinned", pinned_lat);

    return 0;
}