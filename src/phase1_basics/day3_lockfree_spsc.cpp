#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <cassert>

// Simulated market tick payload
struct Tick {
    double price;
    int volume;
    int64_t timestamp_ns;
};

constexpr size_t QUEUE_CAPACITY = 1 << 16; // 65,536 (must be power of 2)
static_assert((QUEUE_CAPACITY & (QUEUE_CAPACITY - 1)) == 0, "Capacity must be power of 2");
constexpr size_t MASK = QUEUE_CAPACITY - 1;
constexpr size_t NUM_TICKS = 10'000'000;

// 🔒 Mutex-backed circular buffer
template<typename T, size_t Cap>
class MutexQueue {
    std::vector<T> buffer_;
    size_t head_ = 0, tail_ = 0;
    std::mutex mtx_;
public:
    MutexQueue() : buffer_(Cap) {}
    bool push(const T& item) {
        std::lock_guard<std::mutex> lock(mtx_);
        if ((head_ + 1) & MASK == (tail_ & MASK)) return false; // full
        buffer_[head_ & MASK] = item;
        head_++;
        return true;
    }
    bool pop(T& item) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (head_ == tail_) return false; // empty
        item = buffer_[tail_ & MASK];
        tail_++;
        return true;
    }
};

// ⚡ Lock-free SPSC Ring Buffer
template<typename T, size_t Cap>
class SPSCQueue {
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T buffer_[Cap];
public:
    bool push(const T& item) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (((h + 1) & MASK) == (tail_.load(std::memory_order_acquire))) return false;
        buffer_[h & MASK] = item;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& item) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        item = buffer_[t & MASK];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
};

template<typename Q>
void benchmark(const char* name, Q& q) {
    Tick dummy{150.25, 100, 0};
    Tick out;
    size_t pushed = 0, popped = 0;

    auto start = std::chrono::high_resolution_clock::now();

    std::thread producer([&]() {
        for (size_t i = 0; i < NUM_TICKS; ++i) {
            dummy.timestamp_ns = i;
            while (!q.push(dummy)) std::this_thread::yield();
        }
    });

    std::thread consumer([&]() {
        while (popped < NUM_TICKS) {
            if (q.pop(out)) {
                ++popped;
                assert(out.timestamp_ns == popped - 1); // Verify ordering
            }
        }
    });

    producer.join();
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << name << ": " << ms << " ms | Throughput: " << (NUM_TICKS / (ms / 1000.0) / 1e6) << "M ticks/sec\n";
}

int main() {
    std::cout << "=== Lock-Free SPSC vs Mutex Queue ===\n";
    std::cout << "Items: " << NUM_TICKS << "\n\n";

    MutexQueue<Tick, QUEUE_CAPACITY> mutex_q;
    benchmark("Mutex Queue", mutex_q);

    SPSCQueue<Tick, QUEUE_CAPACITY> spsc_q;
    benchmark("Lock-Free SPSC", spsc_q);

    return 0;
}