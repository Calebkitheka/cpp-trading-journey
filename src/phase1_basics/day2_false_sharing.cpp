#include <iostream>
#include <thread>
#include <chrono>
#include <cstdint>
#include <atomic>

constexpr size_t ITERATIONS = 500'000'000;

// ❌ Packed: Both counters likely share the same 64-byte cache line
struct PackedCounters {
    std::atomic<uint64_t> counterA{0};
    std::atomic<uint64_t> counterB{0};
};

// ✅ Aligned: Each counter guaranteed its own cache line
struct AlignedCounters {
    alignas(64) std::atomic<uint64_t> counterA{0};
    alignas(64) std::atomic<uint64_t> counterB{0};
};

template<typename T>
void run_benchmark(const char* name, T& counters) {
    auto start = std::chrono::high_resolution_clock::now();

    std::thread t1([&]() {
        for (size_t i = 0; i < ITERATIONS; ++i) 
            counters.counterA.fetch_add(1, std::memory_order_relaxed);
    });

    std::thread t2([&]() {
        for (size_t i = 0; i < ITERATIONS; ++i) 
            counters.counterB.fetch_add(1, std::memory_order_relaxed);
    });

    t1.join();
    t2.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << name << ": " << ms << " ms | A=" << counters.counterA 
              << ", B=" << counters.counterB << "\n";
}

int main() {
    std::cout << "=== False Sharing Benchmark ===\n";
    std::cout << "Iterations per thread: " << ITERATIONS << "\n";
    std::cout << "Expected cache line size: 64 bytes\n\n";

    PackedCounters packed;
    run_benchmark("Packed (False Sharing)", packed);

    AlignedCounters aligned;
    run_benchmark("Aligned (No False Sharing)", aligned);

    return 0;
}