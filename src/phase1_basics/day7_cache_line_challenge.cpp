#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <cstddef>

// ❌ NAIVE LAYOUT: Compiler inserts padding, exceeds 1 cache line
struct NaiveSnapshot {
    uint64_t timestamp_ns;   // 8 bytes (offset 0)
    double   bid_price;      // 8 bytes (offset 8)
    double   ask_price;      // 8 bytes (offset 16)
    int32_t  bid_size;       // 4 bytes (offset 24)
    int32_t  ask_size;       // 4 bytes (offset 28)
    uint32_t seq_id;         // 4 bytes (offset 32)
    uint16_t checksum;       // 2 bytes (offset 36)
    uint8_t  flags;          // 1 byte  (offset 38)
    // Compiler pads to 8-byte alignment (double) → sizeof = 40 or 48 depending on platform
};

// ✅ OPTIMIZED LAYOUT: Exactly 64 bytes, zero padding, SoA-style for hot paths
struct alignas(64) OptimizedSnapshot {
    uint32_t bid_prices[3];   // 12 bytes (0-11)
    uint32_t ask_prices[3];   // 12 bytes (12-23)
    uint32_t bid_sizes[3];    // 12 bytes (24-35)
    uint32_t ask_sizes[3];    // 12 bytes (36-47)
    uint32_t timestamp_us;    // 4 bytes  (48-51)
    uint32_t sequence_id;     // 4 bytes  (52-55)
    uint32_t checksum;        // 4 bytes  (56-59)
    uint8_t  flags;           // 1 byte   (60)
    uint8_t  _pad[3];         // 3 bytes  (61-63)
};
static_assert(sizeof(OptimizedSnapshot) == 64, "Must fit in exactly 1 cache line!");
static_assert(alignof(OptimizedSnapshot) == 64, "Must align to cache boundary!");

// Helper: Print field offsets to visualize layout
void print_layout() {
    std::cout << "=== Memory Layout (Offsets) ===\n";
    std::cout << "bid_prices:  " << offsetof(OptimizedSnapshot, bid_prices) << "\n";
    std::cout << "ask_prices:  " << offsetof(OptimizedSnapshot, ask_prices) << "\n";
    std::cout << "bid_sizes:   " << offsetof(OptimizedSnapshot, bid_sizes) << "\n";
    std::cout << "ask_sizes:   " << offsetof(OptimizedSnapshot, ask_sizes) << "\n";
    std::cout << "timestamp_us:" << offsetof(OptimizedSnapshot, timestamp_us) << "\n";
    std::cout << "sequence_id: " << offsetof(OptimizedSnapshot, sequence_id) << "\n";
    std::cout << "checksum:    " << offsetof(OptimizedSnapshot, checksum) << "\n";
    std::cout << "flags:       " << offsetof(OptimizedSnapshot, flags) << "\n";
    std::cout << "Total size:  " << sizeof(OptimizedSnapshot) << " bytes\n\n";
}

template<typename T>
void benchmark(const char* name, const std::vector<T>& data) {
    volatile int64_t sum = 0;
    auto start = std::chrono::high_resolution_clock::now();
    for (const auto& snap : data) {
        // Simulate hot path: scan all bid prices across depth levels
        for (int i = 0; i < 3; ++i) sum += snap.bid_prices[i];
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << name << " | sizeof=" << sizeof(T) << "B | Time=" << ms << " ms\n";
}

int main() {
    print_layout();
    
    constexpr size_t N = 2'000'000; // ~128MB dataset
    std::vector<NaiveSnapshot> naive_data(N);
    std::vector<OptimizedSnapshot> opt_data(N);

    // Fill with deterministic dummy data
    for (size_t i = 0; i < N; ++i) {
        naive_data[i] = {i*100, 15000, 15001, 100, 100, i, 0xAB, 0x01};
        std::memcpy(&opt_data[i].bid_prices, &naive_data[i], sizeof(opt_data[i].bid_prices));
        opt_data[i].timestamp_us = i;
        opt_data[i].sequence_id = i;
        opt_data[i].checksum = 0xDEAD;
        opt_data[i].flags = 0x01;
    }

    std::cout << "=== Benchmark (Sequential Bid Price Scan) ===\n";
    benchmark("Naive Layout  ", naive_data);
    benchmark("Optimal Layout", opt_data);

    return 0;
}