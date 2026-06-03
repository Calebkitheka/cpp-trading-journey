#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <random>
#include <iomanip>

// --- Core Structures ---
struct Order {
    uint64_t id;
    int32_t price;      // Scaled (e.g., cents: 15000 = $150.00)
    int32_t qty;
    int8_t side;        // 1 = bid, -1 = ask
    Order* next = nullptr;
    Order* prev = nullptr;
};

struct PriceLevel {
    int32_t price;
    Order* head = nullptr;
    Order* tail = nullptr;
};

struct TradeEvent {
    uint64_t aggressor_id;
    uint64_t passive_id;
    int32_t price;
    int32_t fill_qty;
};

// --- Matching Engine ---
class MatchingEngine {
    std::vector<Order> pool_;
    std::vector<PriceLevel> bids_, asks_;
    size_t pool_idx_ = 0;
    std::vector<TradeEvent> trades_;
    std::vector<int64_t> latencies_ns_;

public:
    MatchingEngine(size_t capacity) : pool_(capacity) {}

    Order* add_passive(int32_t price, int32_t qty, int8_t side) {
        if (pool_idx_ >= pool_.size()) return nullptr;
        Order& o = pool_[pool_idx_++];
        o.id = pool_idx_; o.price = price; o.qty = qty; o.side = side;
        o.next = o.prev = nullptr;

        auto& levels = (side == 1) ? bids_ : asks_;
        PriceLevel* lvl = nullptr;
        for (auto& l : levels) if (l.price == price) { lvl = &l; break; }
        if (!lvl) { levels.push_back({price, nullptr, nullptr}); lvl = &levels.back(); }

        if (!lvl->head) lvl->head = lvl->tail = &o;
        else { lvl->tail->next = &o; o.prev = lvl->tail; lvl->tail = &o; }
        return &o;
    }

    void match_aggressive(uint64_t aggressor_id, int32_t price, int32_t qty, int8_t side) {
        auto start = std::chrono::high_resolution_clock::now();

        auto& levels = (side == 1) ? asks_ : bids_; // Buy hits asks, sell hits bids
        bool is_buy = (side == 1);
        int remaining = qty;

        for (auto it = levels.begin(); it != levels.end() && remaining > 0; ) {
            // Price-time priority: stop if price no longer crosses
            bool crosses = is_buy ? (it->price <= price) : (it->price >= price);
            if (!crosses) break;

            Order* cur = it->head;
            while (cur && remaining > 0) {
                int fill_qty = std::min(remaining, cur->qty);
                trades_.push_back({aggressor_id, cur->id, cur->price, fill_qty});
                remaining -= fill_qty;
                cur->qty -= fill_qty;

                Order* next = cur->next;
                if (cur->qty == 0) { // Fully filled: remove from level
                    if (cur->prev) cur->prev->next = cur->next;
                    else it->head = cur->next;
                    if (cur->next) cur->next->prev = cur->prev;
                    else it->tail = cur->prev;
                    cur->next = cur->prev = nullptr; // Invalidate
                }
                cur = next;
            }
            if (!it->head) it = levels.erase(it);
            else ++it;
        }

        auto end = std::chrono::high_resolution_clock::now();
        latencies_ns_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    void print_stats() const {
        if (latencies_ns_.empty()) return;
        std::vector<int64_t> sorted = latencies_ns_;
        std::sort(sorted.begin(), sorted.end());

        auto percentile = [&](double p) {
            return sorted[static_cast<size_t>(sorted.size() * p / 100.0)];
        };

        std::cout << "\n=== Matching Engine Latency (ns) ===\n";
        std::cout << "Min:   " << std::setw(8) << sorted.front() << "\n";
        std::cout << "p50:   " << std::setw(8) << percentile(50) << "\n";
        std::cout << "p90:   " << std::setw(8) << percentile(90) << "\n";
        std::cout << "p99:   " << std::setw(8) << percentile(99) << "\n";
        std::cout << "Max:   " << std::setw(8) << sorted.back() << "\n";
        std::cout << "Fills: " << std::setw(8) << trades_.size() << " events\n";
    }
};

int main() {
    constexpr size_t POOL_CAP = 1'000'000;
    MatchingEngine engine(POOL_CAP);
    std::mt19937_64 rng(42);

    // 1️⃣ Seed the book (100 resting limit orders across 10 price levels)
    std::cout << "Seeding order book...\n";
    for (int i = 0; i < 100; ++i) {
        int32_t price = 15000 + (rng() % 10); // $150.00 - $150.09
        int32_t qty = 10 + (rng() % 5) * 10;
        int8_t side = (i % 2 == 0) ? 1 : -1;
        engine.add_passive(price, qty, side);
    }

    // 2️⃣ Send aggressive orders & measure latency
    constexpr int AGGRESSIVE_ORDERS = 50'000;
    std::cout << "Running " << AGGRESSIVE_ORDERS << " aggressive matches...\n";

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < AGGRESSIVE_ORDERS; ++i) {
        int32_t price = 15000 + (rng() % 10);
        int32_t qty = 10 + (rng() % 3) * 10;
        int8_t side = (i % 2 == 0) ? 1 : -1;
        engine.match_aggressive(i + 1000, price, qty, side);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    engine.print_stats();
    std::cout << "Total Runtime: " << total_ms << " ms\n";
    std::cout << "Throughput:    " << static_cast<int>(AGGRESSIVE_ORDERS / (total_ms / 1000.0)) << " matches/sec\n";

    return 0;
}