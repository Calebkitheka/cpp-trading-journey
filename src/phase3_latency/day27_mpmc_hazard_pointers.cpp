#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <cstring>

constexpr int MAX_THREADS = 8;
constexpr int RETIRE_LIMIT = 4 * MAX_THREADS;
constexpr size_t QUEUE_CAP = 1 << 16; // 65,536

// 🔹 Hazard Pointer Registry (Simplified for demo)
class HazardPointer {
    std::atomic<void*> hp_[MAX_THREADS]{};
    std::vector<void*> retired_;
    std::mutex retired_mtx_; // Only for retired list (not in hot path)
public:
    void protect(int tid, void* ptr) { hp_[tid].store(ptr, std::memory_order_release); }
    void clear(int tid) { hp_[tid].store(nullptr, std::memory_order_release); }
    
    void retire(void* ptr) {
        std::lock_guard<std::mutex> lock(retired_mtx_);
        retired_.push_back(ptr);
        if (retired_.size() >= RETIRE_LIMIT) scan_and_reclaim();
    }

    void scan_and_reclaim() {
        for (auto it = retired_.begin(); it != retired_.end(); ) {
            bool safe = true;
            for (int i = 0; i < MAX_THREADS; ++i) {
                if (hp_[i].load(std::memory_order_acquire) == *it) {
                    safe = false; break;
                }
            }
            if (safe) { delete static_cast<std::byte*>(*it); it = retired_.erase(it); }
            else { ++it; }
        }
    }
};

// 🔹 Bounded MPMC Ring Buffer (Vyukov-style simplified)
template<typename T>
class MPMCQueue {
    struct alignas(64) Cell {
        std::atomic<size_t> sequence;
        T data;
        Cell() : sequence(0) {}
    };
    Cell buffer_[QUEUE_CAP];
    std::atomic<size_t> enqueue_pos_{0}, dequeue_pos_{0};
public:
    MPMCQueue() {
        for (size_t i = 0; i < QUEUE_CAP; ++i) buffer_[i].sequence.store(i);
    }

    bool try_push(const T& item) {
        size_t pos;
        Cell* cell;
        while (true) {
            pos = enqueue_pos_.load(std::memory_order_relaxed);
            cell = &buffer_[pos & (QUEUE_CAP - 1)];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)pos;
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) return false; // Queue full
        }
        cell->data = item;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& item) {
        size_t pos;
        Cell* cell;
        while (true) {
            pos = dequeue_pos_.load(std::memory_order_relaxed);
            cell = &buffer_[pos & (QUEUE_CAP - 1)];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) return false; // Queue empty
        }
        item = cell->data;
        cell->sequence.store(pos + QUEUE_CAP, std::memory_order_release);
        return true;
    }
};

struct MarketTick { uint64_t ts; double price; int32_t vol; };

int main() {
    MPMCQueue<MarketTick> queue;
    HazardPointer hp_mgr;
    std::atomic<size_t> produced{0}, consumed{0};
    constexpr size_t TICKS_PER_THREAD = 2'000'000;
    constexpr int PRODUCERS = 4, CONSUMERS = 4;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    
    // 📡 Producers: NIC parsers feeding the bus
    for (int i = 0; i < PRODUCERS; ++i) {
        threads.emplace_back([&, i]() {
            MarketTick tick{0, 150.0, 100};
            for (size_t t = 0; t < TICKS_PER_THREAD; ++t) {
                tick.ts = t * 500 + i * 1000;
                while (!queue.try_push(tick)) std::this_thread::yield();
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 🎯 Consumers: Strategy engines reading from the bus
    for (int i = 0; i < CONSUMERS; ++i) {
        threads.emplace_back([&, i]() {
            MarketTick tick;
            hp_mgr.protect(i, &queue); // Simulate HP registration
            while (consumed.load(std::memory_order_relaxed) < TICKS_PER_THREAD * PRODUCERS) {
                if (queue.try_pop(tick)) {
                    hp_mgr.protect(i, &tick); // Protect during processing
                    // Simulate strategy logic
                    hp_mgr.clear(i);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
            hp_mgr.clear(i);
        });
    }

    for (auto& t : threads) t.join();
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "=== Lock-Free MPMC Queue & Hazard Pointers Benchmark ===\n";
    std::cout << "Producers: " << PRODUCERS << " | Consumers: " << CONSUMERS << "\n";
    std::cout << "Ticks Produced: " << produced.load() << "\n";
    std::cout << "Ticks Consumed: " << consumed.load() << "\n";
    std::cout << "Wall Time     : " << ms << " ms\n";
    std::cout << "Throughput    : " << (consumed.load() / (ms / 1000.0) / 1e6) << "M ticks/sec\n\n";

    std::cout << "📊 MPMC & Hazard Pointer Insights:\n";
    std::cout << " • Bounded queues avoid heap allocation → deterministic latency\n";
    std::cout << " • CAS loops replace mutexes → zero kernel transitions\n";
    std::cout << " • Hazard Pointers prevent ABA: readers protect nodes before CAS updates\n";
    std::cout << " • Production: HFT firms combine bounded rings + EBR/HP for safe memory reuse\n";

    return 0;
}