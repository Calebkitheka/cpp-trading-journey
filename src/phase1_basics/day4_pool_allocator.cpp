#include <iostream>
#include <vector>
#include <chrono>
#include <array>
#include <new>
#include <cassert>
#include <cstddef>

// Realistic limit order payload (~32 bytes)
struct LimitOrder {
    int64_t order_id;
    double price;
    int32_t quantity;
    int8_t side;       // 1 = buy, -1 = sell
    uint64_t timestamp_ns;
};

constexpr size_t NUM_ORDERS = 5'000'000;
constexpr size_t POOL_CAPACITY = NUM_ORDERS;

// 🔒 Standard heap allocation
void benchmark_new_delete() {
    auto start = std::chrono::high_resolution_clock::now();
    std::vector<LimitOrder*> orders;
    orders.reserve(NUM_ORDERS);

    for (size_t i = 0; i < NUM_ORDERS; ++i) {
        orders.push_back(new LimitOrder{static_cast<int64_t>(i), 150.0 + i * 0.01, 100, 1, 0});
    }

    long long checksum = 0;
    for (auto* o : orders) checksum += o->order_id;
    for (auto* o : orders) delete o;

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "new/delete: " << ms << " ms | checksum=" << checksum << "\n";
}

// ⚡ Deterministic Object Pool (Intrusive Free List)
template<typename T, size_t Capacity>
class ObjectPool {
    alignas(64) std::array<std::byte, Capacity * sizeof(T)> storage_;
    alignas(64) std::array<size_t, Capacity> next_idx_; // Free list indices
    size_t free_head_ = 0;
    size_t alloc_count_ = 0;

public:
    ObjectPool() {
        // Link all blocks into a free list
        for (size_t i = 0; i < Capacity - 1; ++i) next_idx_[i] = i + 1;
        next_idx_[Capacity - 1] = Capacity; // Sentinel
    }

    T* allocate() {
        if (free_head_ == Capacity) return nullptr; // Pool exhausted
        size_t idx = free_head_;
        free_head_ = next_idx_[idx];
        ++alloc_count_;
        // Placement new: construct object in pre-allocated memory
        return new (storage_.data() + idx * sizeof(T)) T{};
    }

    void deallocate(T* ptr) {
        // Calculate index from pointer arithmetic
        size_t idx = (reinterpret_cast<std::byte*>(ptr) - storage_.data()) / sizeof(T);
        assert(idx < Capacity && "Pointer not from this pool!");
        
        ptr->~T(); // Explicit destructor call
        next_idx_[idx] = free_head_;
        free_head_ = idx;
        --alloc_count_;
    }

    size_t active_count() const { return alloc_count_; }
};

// 🔹 Pool benchmark
void benchmark_pool() {
    auto start = std::chrono::high_resolution_clock::now();
    ObjectPool<LimitOrder, POOL_CAPACITY> pool;
    std::vector<LimitOrder*> orders;
    orders.reserve(NUM_ORDERS);

    for (size_t i = 0; i < NUM_ORDERS; ++i) {
        LimitOrder* o = pool.allocate();
        // Re-initialize (placement new already called constructor above)
        o->order_id = i;
        o->price = 150.0 + i * 0.01;
        o->quantity = 100;
        o->side = 1;
        orders.push_back(o);
    }

    long long checksum = 0;
    for (auto* o : orders) checksum += o->order_id;
    for (auto* o : orders) pool.deallocate(o);

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "ObjectPool:   " << ms << " ms | checksum=" << checksum << " | active=" << pool.active_count() << "\n";
}

int main() {
    std::cout << "=== Standard Allocator vs Object Pool ===\n";
    std::cout << "Orders: " << NUM_ORDERS << "\n\n";
    benchmark_new_delete();
    benchmark_pool();
    return 0;
}