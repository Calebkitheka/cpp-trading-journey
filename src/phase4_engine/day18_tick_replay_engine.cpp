#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <random>
#include <iomanip>
#include <algorithm>
#include <cassert>

constexpr size_t NUM_TICKS = 10'000'000;
constexpr size_t SNAPSHOT_INTERVAL = 500'000;
constexpr size_t MAX_ORDER_IDS = 2'000'000;

enum class TickType : uint8_t { ADD = 1, MODIFY = 2, CANCEL = 3, EXECUTE = 4 };

struct alignas(32) MarketTick {
    uint64_t seq;
    uint64_t order_id;
    int32_t price;    // Scaled: 15000 = $150.00
    int32_t qty;
    TickType type;
};

// 🔹 Sequence Validator & Gap Recovery Simulator
class SequenceValidator {
    uint64_t expected_seq_ = 1;
    size_t gaps_detected_ = 0;
    size_t duplicates_skipped_ = 0;
public:
    enum class Status { OK, GAP, DUPLICATE };

    Status validate(uint64_t seq) {
        if (seq == expected_seq_) {
            ++expected_seq_;
            return Status::OK;
        }
        if (seq > expected_seq_) {
            ++gaps_detected_;
            expected_seq_ = seq + 1; // Simulate retransmission catch-up
            return Status::GAP;
        }
        ++duplicates_skipped_;
        return Status::DUPLICATE;
    }
    size_t gaps() const { return gaps_detected_; }
    size_t duplicates() const { return duplicates_skipped_; }
};

// 🔹 Simplified L2 Book State for Replay
struct BookSnapshot {
    int64_t bid_volume = 0;
    int64_t ask_volume = 0;
    int32_t best_bid = 0;
    int32_t best_ask = 999999;
    uint64_t active_orders = 0;
    uint64_t processed_ticks = 0;
};

// Order tracking pool (replaces dynamic map for latency)
struct OrderState { int32_t price = 0; int32_t qty = 0; int8_t side = 0; };

class ReplayEngine {
    SequenceValidator seq_validator_;
    std::vector<OrderState> orders_; // Indexed by order_id % MAX_ORDER_IDS
    BookSnapshot current_state_;
    std::vector<BookSnapshot> history_;

public:
    ReplayEngine() : orders_(MAX_ORDER_IDS) {
        history_.reserve(NUM_TICKS / SNAPSHOT_INTERVAL);
    }

    void process(const MarketTick& t) {
        auto status = seq_validator_.validate(t.seq);
        if (status != SequenceValidator::Status::OK) return; // Skip gaps/dupes in hot path

        OrderState& ord = orders_[t.order_id % MAX_ORDER_IDS];
        int32_t prev_qty = ord.qty;

        switch (t.type) {
            case TickType::ADD:
                ord.price = t.price; ord.qty = t.qty; ord.side = (t.price < 15000) ? 1 : -1;
                break;
            case TickType::MODIFY:
                ord.qty = t.qty;
                break;
            case TickType::CANCEL:
            case TickType::EXECUTE:
                ord.qty = 0;
                break;
        }

        // Incremental volume update
        int32_t delta = ord.qty - prev_qty;
        if (delta != 0) {
            if (ord.side == 1) current_state_.bid_volume += delta;
            else current_state_.ask_volume += delta;
            
            if (ord.side == 1 && t.price > current_state_.best_bid && ord.qty > 0)
                current_state_.best_bid = t.price;
            if (ord.side == -1 && t.price < current_state_.best_ask && ord.qty > 0)
                current_state_.best_ask = t.price;
        }

        ++current_state_.processed_ticks;
        if (current_state_.processed_ticks % SNAPSHOT_INTERVAL == 0) {
            history_.push_back(current_state_);
        }
    }

    const std::vector<BookSnapshot>& get_history() const { return history_; }
    size_t get_gaps() const { return seq_validator_.gaps(); }
    size_t get_dupes() const { return seq_validator_.duplicates(); }
};

// 📡 Simulate raw multicast UDP stream with intentional gaps & duplicates
std::vector<MarketTick> generate_raw_stream(size_t count) {
    std::vector<MarketTick> stream;
    stream.reserve(count);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> price_dist(14950, 15050);
    std::uniform_int_distribution<int> qty_dist(10, 500);
    std::uniform_int_distribution<int> type_dist(1, 4);

    uint64_t seq = 1;
    for (size_t i = 0; i < count; ++i) {
        // 1% gap, 0.5% duplicate, rest normal
        if (rng() % 1000 < 10) { seq += 2; continue; } // Simulate lost packet
        if (rng() % 1000 < 5 && i > 0) { stream.push_back(stream.back()); continue; }

        MarketTick t{seq++, static_cast<uint64_t>(rng()), price_dist(rng), qty_dist(rng), static_cast<TickType>(type_dist(rng))};
        stream.push_back(t);
    }
    return stream;
}

int main() {
    std::cout << "=== Order Book Reconstruction & Deterministic Replay ===\n";
    std::cout << "Ticks: " << NUM_TICKS << "\n";
    std::cout << "Snapshot Interval: " << SNAPSHOT_INTERVAL << "\n\n";

    auto raw_stream = generate_raw_stream(NUM_TICKS);
    ReplayEngine engine;

    auto start = std::chrono::high_resolution_clock::now();
    for (const auto& t : raw_stream) engine.process(t);
    auto end = std::chrono::high_resolution_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    const auto& hist = engine.get_history();

    std::cout << "Processing Time      : " << ms << " ms\n";
    std::cout << "Throughput           : " << std::fixed << std::setprecision(1) 
              << (raw_stream.size() / (ms / 1000.0) / 1e6) << "M ticks/sec\n";
    std::cout << "Sequence Gaps Handled: " << engine.get_gaps() << "\n";
    std::cout << "Duplicates Skipped   : " << engine.get_dupes() << "\n";
    std::cout << "Snapshots Captured   : " << hist.size() << "\n\n";

    std::cout << "Sample Book State (Last Snapshot):\n";
    const auto& last = hist.back();
    std::cout << "  Best Bid   : $" << last.best_bid / 100.0 << "\n";
    std::cout << "  Best Ask   : $" << last.best_ask / 100.0 << "\n";
    std::cout << "  Spread     : " << (last.best_ask - last.best_bid) / 100.0 << "\n";
    std::cout << "  Bid Vol    : " << last.bid_volume << "\n";
    std::cout << "  Ask Vol    : " << last.ask_volume << "\n";

    std::cout << "\n📊 Why This Matters for Backtesting:\n";
    std::cout << " • Deterministic: Same raw stream → identical book state across runs\n";
    std::cout << " • Incremental: O(1) updates per tick, no full rebuild\n";
    std::cout << " • Gap-Tolerant: Handles real multicast loss without stalling\n";
    std::cout << " • Production: NASDAQ ITCH/CME FAST use identical sequence-driven replay\n";

    return 0;
}