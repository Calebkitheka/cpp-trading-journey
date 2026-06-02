#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <cassert>
#include <random>
#include <iomanip>

constexpr size_t POOL_CAPACITY = 1'000'000;
constexpr int    NUM_PRICE_LEVELS = 20;

// ❌ Standard approach: std::list<Order> allocates a node per order + stores data
// ✅ Intrusive approach: Order *is* the node. Links live inside the payload.

struct Order {
    uint64_t id;
    int32_t  price;      // Scaled integer (e.g., cents: 15000 = $150.00)
    int32_t  qty;
    int64_t  time_ns;
    int8_t   side;       // 1 = bid, -1 = ask
    Order*   next = nullptr;
    Order*   prev = nullptr;
};

struct PriceLevel {
    int32_t  price;
    int64_t  volume = 0;
    Order*   head = nullptr;
    Order*   tail = nullptr;
};

class IntrusiveLOB {
    std::vector<Order> pool_;
    std::vector<PriceLevel> levels_;
    size_t alloc_idx_ = 0;

public:
    IntrusiveLOB() : pool_(POOL_CAPACITY), levels_() {
        levels_.reserve(NUM_PRICE_LEVELS);
    }

    // Add to price level. O(1) if level exists, O(P) if creating new level
    Order* add_order(int32_t price, int32_t qty, int64_t time_ns, int8_t side) {
        if (alloc_idx_ >= POOL_CAPACITY) return nullptr;
        Order& ord = pool_[alloc_idx_++];
        ord.id = alloc_idx_;
        ord.price = price;
        ord.qty = qty;
        ord.time_ns = time_ns;
        ord.side = side;
        ord.next = ord.prev = nullptr;

        PriceLevel* lvl = nullptr;
        for (auto& l : levels_) {
            if (l.price == price) { lvl = &l; break; }
        }
        if (!lvl) {
            levels_.push_back({price, 0, nullptr, nullptr});
            lvl = &levels_.back();
        }

        // FIFO append (price-time priority)
        lvl->volume += qty;
        if (!lvl->head) {
            lvl->head = lvl->tail = &ord;
        } else {
            lvl->tail->next = &ord;
            ord.prev = lvl->tail;
            lvl->tail = &ord;
        }
        return &ord;
    }

    // O(1) cancel when holding the Order* pointer
    void cancel_order(Order* ord) {
        if (!ord || ord->qty == 0) return;

        if (ord->prev) ord->prev->next = ord->next;
        else {
            for (auto& l : levels_) if (l.head == ord) { l.head = ord->next; break; }
        }

        if (ord->next) ord->next->prev = ord->prev;
        else {
            for (auto& l : levels_) if (l.tail == ord) { l.tail = ord->prev; break; }
        }

        // Reset state
        ord->next = ord->prev = nullptr;
        ord->qty = 0;
    }

    size_t get_levels_count() const { return levels_.size(); }
};

int main() {
    constexpr int ITERATIONS = 500'000;
    IntrusiveLOB lob;
    std::vector<Order*> active_orders;
    active_orders.reserve(ITERATIONS);
    std::mt19937_64 rng(42);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        int32_t price = 15000 + (rng() % NUM_PRICE_LEVELS);
        int32_t qty = 100 + (rng() % 5) * 10;
        int64_t time = i * 1000; // ~1μs between ticks
        int8_t side = (rng() % 2 == 0) ? 1 : -1;

        Order* ord = lob.add_order(price, qty, time, side);
        active_orders.push_back(ord);

        // Simulate market churn: cancel ~30% of recent orders
        if (i > 200 && rng() % 3 == 0) {
            size_t cancel_idx = rng() % active_orders.size();
            lob.cancel_order(active_orders[cancel_idx]);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double ops_per_sec = (ITERATIONS * 1000.0) / (ms == 0 ? 1 : ms);

    std::cout << "=== Intrusive LOB Benchmark ===\n";
    std::cout << "Iterations:       " << std::setw(8) << ITERATIONS << "\n";
    std::cout << "Time:             " << std::setw(8) << ms << " ms\n";
    std::cout << "Ops/sec:          " << std::setw(8) << static_cast<int>(ops_per_sec) << "K\n";
    std::cout << "Price Levels:     " << std::setw(8) << lob.get_levels_count() << "\n";
    std::cout << "Pool Used:        " << std::setw(8) << (lob.pool_.size() - POOL_CAPACITY + 1'000'000) << " / " << POOL_CAPACITY << "\n";

    return 0;
}