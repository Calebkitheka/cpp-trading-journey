#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <random>
#include <iomanip>

enum class Regime { Normal, Volatile, Halt };
enum class OrderType { Limit, Market, IOC, Stop };

struct alignas(32) Order {
    Regime regime;
    OrderType type;
    double price;
    int32_t qty;
};

// Simulated hot-path handlers
volatile int64_t sink = 0;
inline void handle_normal(const Order& o) { sink += o.qty * 100; }
inline void handle_volatile(const Order& o) { sink += o.qty * 200; }
inline void handle_ioc(const Order& o) { sink += o.qty * 50; }
inline void handle_stop(const Order& o) { sink += o.qty * 300; }
inline void handle_halt(const Order& o) { sink += 0; }

// 🔹 BASELINE: Naive branching order
void route_baseline(const Order& o) {
    if (o.regime == Regime::Halt) handle_halt(o);
    else if (o.type == OrderType::Stop) handle_stop(o);
    else if (o.regime == Regime::Volatile) handle_volatile(o);
    else if (o.type == OrderType::IOC) handle_ioc(o);
    else handle_normal(o);
}

// 🔹 C++20 HINTS: Manual branch prediction hints
void route_hinted(const Order& o) {
    if (o.regime == Regime::Halt) [[unlikely]] handle_halt(o);
    else if (o.type == OrderType::Stop) [[unlikely]] handle_stop(o);
    else if (o.regime == Regime::Volatile) handle_volatile(o);
    else if (o.type == OrderType::IOC) handle_ioc(o);
    else [[likely]] handle_normal(o);
}

// Generate realistic market distribution (70/20/7/2/1)
std::vector<Order> generate_market_data(size_t count) {
    std::vector<Order> data;
    data.reserve(count);
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> p_dist(140.0, 160.0);
    std::uniform_int_distribution<int> q_dist(10, 500);

    std::uniform_int_distribution<int> regime_dist(0, 99);
    std::uniform_int_distribution<int> type_dist(0, 99);

    for (size_t i = 0; i < count; ++i) {
        Regime r = regime_dist(rng) < 99 ? (regime_dist(rng) < 80 ? Regime::Normal : Regime::Volatile) : Regime::Halt;
        OrderType t = static_cast<OrderType>(type_dist(rng) % 4);
        data.push_back({r, t, p_dist(rng), q_dist(rng)});
    }
    return data;
}

template<typename Fn>
double benchmark(const char* name, Fn fn, const std::vector<Order>& data) {
    auto start = std::chrono::high_resolution_clock::now();
    for (const auto& o : data) fn(o);
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
}

int main() {
    constexpr size_t N = 10'000'000;
    auto data = generate_market_data(N);

    std::cout << "=== Branch Prediction & PGO Benchmark ===\n";
    std::cout << "Orders: " << N << " (70% Normal, 20% Volatile, 7% IOC, 2% Stop, 1% Halt)\n\n";

    double ms_base = benchmark("Baseline", route_baseline, data);
    double ms_hint = benchmark("C++20 Hints", route_hinted, data);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << std::left << std::setw(18) << "Baseline     : " << ms_base << " ms\n";
    std::cout << std::left << std::setw(18) << "C++20 Hints  : " << ms_hint << " ms\n";
    std::cout << "Speedup        : " << (ms_base / std::max(0.1, ms_hint)) << "x\n";
    std::cout << "Sink Checksum  : " << sink << "\n\n";

    std::cout << "📊 PGO Workflow (run separately):\n";
    std::cout << "GCC/Clang:\n";
    std::cout << "  1. g++ -std=c++20 -O3 -fprofile-generate -march=native -o pgo_gen day13_pgo_branch_prediction.cpp\n";
    std::cout << "  2. ./pgo_gen\n";
    std::cout << "  3. g++ -std=c++20 -O3 -fprofile-use -march=native -o pgo_opt day13_pgo_branch_prediction.cpp\n";
    std::cout << "  4. ./pgo_opt  # Expect 10-25% faster + lower branch-miss rate\n";
    std::cout << "\nMSVC:\n";
    std::cout << "  1. cl /std:c++20 /O2 /GL /LTCG:PGINSTRUMENT ...\n";
    std::cout << "  2. run instrumented binary\n";
    std::cout << "  3. cl /std:c++20 /O2 /GL /LTCG:PGUPDATE ...\n";
    std::cout << "  4. run optimized binary\n";

    return 0;
}