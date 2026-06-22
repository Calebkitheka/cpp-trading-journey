#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <random>
#include <iomanip>
#include <algorithm>

constexpr size_t N = 10'000'000;
constexpr uint32_t TARGET_SYMBOL = 42;

// ❌ AoS (Array of Structures): Traditional C++ layout
// Mixed types → each struct spans ~24-32 bytes. Filtering by symbol_id
// forces CPU to load price, ts, qty into cache even when unused.
struct alignas(32) TickAoS {
    uint64_t ts_ns;      // 8B
    double   price;      // 8B
    int32_t  qty;        // 4B
    uint32_t symbol_id;  // 4B
    uint8_t  side;       // 1B
    uint8_t  _pad[7];    // Padding to 32B boundary
};

// ✅ SoA (Structure of Arrays): Columnar layout
// Each field is contiguous. Filtering only touches symbol_id array first.
// Enables SIMD vectorization & eliminates cache pollution.
template<typename T>
struct alignas(64) AlignedVector {
    T* data_ = nullptr;
    size_t size_ = 0;
    
    AlignedVector(size_t n) {
        size_ = n;
        // Guarantee 64B alignment for cache line optimization
        #if defined(_WIN32)
        data_ = static_cast<T*>(_aligned_malloc(n * sizeof(T), 64));
        #else
        data_ = static_cast<T*>(aligned_alloc(64, n * sizeof(T)));
        #endif
    }
    ~AlignedVector() {
        #if defined(_WIN32)
        _aligned_free(data_);
        #else
        free(data_);
        #endif
    }
    T& operator[](size_t i) { return data_[i]; }
    const T& operator[](size_t i) const { return data_[i]; }
};

struct TickSoA {
    AlignedVector<uint64_t> ts_ns;
    AlignedVector<double>   price;
    AlignedVector<int32_t>  qty;
    AlignedVector<uint32_t> symbol_id;
    AlignedVector<uint8_t>  side;

    TickSoA(size_t n) : ts_ns(n), price(n), qty(n), symbol_id(n), side(n) {}
};

// Generate identical datasets for fair comparison
void gen_data(std::vector<TickAoS>& aos, TickSoA& soa, size_t n) {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint32_t> sym_dist(1, 100);
    std::uniform_int_distribution<int32_t> qty_dist(10, 1000);
    std::uniform_int_distribution<uint8_t> side_dist(0, 1);

    for (size_t i = 0; i < n; ++i) {
        uint32_t sym = sym_dist(rng);
        aos.push_back({
            i * 1000, 
            150.0 + (rng() % 1000) * 0.01, 
            qty_dist(rng), 
            sym, 
            side_dist(rng), 
            {0}
        });
        soa.ts_ns[i] = i * 1000;
        soa.price[i] = 150.0 + (rng() % 1000) * 0.01;
        soa.qty[i] = qty_dist(rng);
        soa.symbol_id[i] = sym;
        soa.side[i] = side_dist(rng);
    }
}

template<typename Fn>
double benchmark(const char* name, Fn fn) {
    auto start = std::chrono::high_resolution_clock::now();
    fn();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

int main() {
    std::vector<TickAoS> aos_data; aos_data.reserve(N);
    TickSoA soa_data(N);
    gen_data(aos_data, soa_data, N);

    volatile double sink_price = 0.0;
    volatile int64_t sink_vol = 0;

    std::cout << "=== AoS vs SoA Cache Layout Benchmark ===\n";
    std::cout << "Ticks: " << N << " | Target Symbol: " << TARGET_SYMBOL << " (~1% match rate)\n\n";

    // 🔹 AoS: Row-major traversal
    double ms_aos = benchmark("AoS (Row-Major)", [&]() {
        for (const auto& t : aos_data) {
            if (t.symbol_id == TARGET_SYMBOL) [[unlikely]] {
                sink_price += t.price;
                sink_vol += t.qty;
            }
        }
    });

    // 🔹 SoA: Columnar traversal (filter IDs first, gather price/vol only on hit)
    double ms_soa = benchmark("SoA (Columnar)", [&]() {
        for (size_t i = 0; i < N; ++i) {
            if (soa_data.symbol_id[i] == TARGET_SYMBOL) [[unlikely]] {
                sink_price += soa_data.price[i];
                sink_vol += soa_data.qty[i];
            }
        }
    });

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "AoS (Row-Major) : " << ms_aos << " ms\n";
    std::cout << "SoA (Columnar)  : " << ms_soa << " ms\n";
    std::cout << "Speedup         : " << (ms_aos / std::max(0.1, ms_soa)) << "x\n\n";

    std::cout << "📊 Cache Architecture Insights:\n";
    std::cout << " • AoS: 32B/row. Loading symbol_id drags price/ts/qty into L1 → 70% cache waste.\n";
    std::cout << " • SoA: 4B/ID. CPU prefetches contiguous IDs → 8x less memory bandwidth.\n";
    std::cout << " • Vectorization: SoA enables `_mm256_cmpeq_epi32` (16 IDs/cycle). AoS needs `_mm256_i32gather_pd` (slow).\n";
    std::cout << " • Production: Order books, risk engines, and tick databases use SoA for analytical queries.\n\n";
    std::cout << "🔍 Profile cache misses (run separately):\n";
    std::cout << " Linux:  perf stat -e L1-dcache-load-misses,L1-dcache-loads ./day25_aos_vs_soa\n";
    std::cout << " Windows: wprui -> Run -> Save trace -> Open in WPA -> Cache Miss Analysis\n";

    return 0;
}
