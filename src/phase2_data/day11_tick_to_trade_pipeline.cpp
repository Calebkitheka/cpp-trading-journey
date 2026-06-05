#include <iostream>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <cstdint>
#include <algorithm>
#include <random>
#include <iomanip>

// 1️⃣ TICK PAYLOAD: Align to 64B to prevent false sharing across cores
struct alignas(64) MarketTick {
    int64_t ingress_ns;   // Pipeline entry timestamp (RDTSC/QPC equivalent)
    int64_t timestamp_ms; // Exchange timestamp
    double  price;
    int32_t volume;
    int8_t  side;         // 1 = buy, -1 = sell
    int8_t  _pad[3];      // Explicit padding for deterministic layout
};

// 2️⃣ LOCK-FREE SPSC RING BUFFER
template<typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;
    
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T buffer_[Capacity];

public:
    bool push(const T& item) {
        size_t h = head_.load(std::memory_order_relaxed);
        if (((h + 1) & MASK) == (tail_.load(std::memory_order_acquire))) return false;
        buffer_[h & MASK] = item;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        item = buffer_[t & MASK];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
};

// 3️⃣ CONSUMER: Simulated Risk/Matching Logic
inline void process_tick(const MarketTick& tick, volatile int64_t& sink) {
    // Simulate pre-trade check + price alignment
    sink += static_cast<int64_t>(tick.price * 100) * tick.volume;
}

int main() {
    constexpr size_t NUM_TICKS = 5'000'000;
    constexpr size_t QUEUE_CAP = 1 << 15; // 32,768 (fits in L2 cache)
    SPSCQueue<MarketTick, QUEUE_CAP> pipeline;
    volatile int64_t sink = 0; // Prevents compiler dead-code elimination

    std::vector<int64_t> latencies_ns;
    latencies_ns.reserve(NUM_TICKS);

    std::atomic<size_t> produced{0};
    std::atomic<size_t> consumed{0};

    auto wall_start = std::chrono::high_resolution_clock::now();

    // 📡 PRODUCER THREAD (Simulates NIC/Parser feeding the ring buffer)
    std::thread producer([&]() {
        std::mt19937_64 rng(42);
        for (size_t i = 0; i < NUM_TICKS; ++i) {
            MarketTick tick{};
            tick.ingress_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count();
            tick.timestamp_ms = i;
            tick.price = 150.0 + (rng() % 100) * 0.01;
            tick.volume = 100 + rng() % 50;
            tick.side = (rng() % 2 == 0) ? 1 : -1;

            while (!pipeline.push(tick)) std::this_thread::yield();
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // 🎯 CONSUMER THREAD (Simulates Matching Engine/Risk Gateway)
    std::thread consumer([&]() {
        MarketTick tick;
        while (consumed.load(std::memory_order_relaxed) < NUM_TICKS) {
            if (pipeline.pop(tick)) {
                auto t_start = std::chrono::high_resolution_clock::now();
                process_tick(tick, sink);
                auto t_end = std::chrono::high_resolution_clock::now();

                int64_t e2e_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
                latencies_ns.push_back(e2e_ns);
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield(); // Backoff when queue empty
            }
        }
    });

    producer.join();
    consumer.join();
    auto wall_end = std::chrono::high_resolution_clock::now();

    // 📊 LATENCY ANALYSIS
    std::sort(latencies_ns.begin(), latencies_ns.end());
    auto p = [&](double pct) { return latencies_ns[static_cast<size_t>(latencies_ns.size() * pct / 100.0)]; };

    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count();

    std::cout << "=== End-to-End Tick-to-Trade Pipeline ===\n";
    std::cout << "Total Ticks      : " << std::setw(8) << NUM_TICKS << "\n";
    std::cout << "Wall Clock Time  : " << std::setw(8) << wall_ms << " ms\n";
    std::cout << "Throughput       : " << std::setw(8) << (NUM_TICKS / (wall_ms / 1000.0) / 1e6) << "M ticks/sec\n\n";
    std::cout << "Processing Latency (ns):\n";
    std::cout << "  Min  : " << std::setw(8) << p(0)   << "\n";
    std::cout << "  p50  : " << std::setw(8) << p(50)  << "\n";
    std::cout << "  p90  : " << std::setw(8) << p(90)  << "\n";
    std::cout << "  p99  : " << std::setw(8) << p(99)  << "\n";
    std::cout << "  Max  : " << std::setw(8) << p(100) << "\n";
    std::cout << "Sink Checksum    : " << sink << " (prevents optimization)\n";

    return 0;
}