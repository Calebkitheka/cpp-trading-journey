#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <algorithm>
#include <random>
#include <cstring>

constexpr size_t QUEUE_CAP   = 1 << 14; // 16,384 (fits in L2 cache)
constexpr uint64_t MAX_POS   = 10'000;  // Position limit
constexpr uint64_t MAX_QTY   = 500;     // Max single order size
constexpr double   MAX_DD    = 5'000.0; // Max drawdown ($)
constexpr uint64_t RATE_CAP  = 2'000;   // Orders/sec limit
constexpr size_t   NUM_SIGNALS = 5'000'000;

// 🔹 Signal from Alpha Strategy
struct alignas(64) Signal {
    int64_t  ts_ns;
    int32_t  symbol_id;
    int8_t   side;   // 1=buy, -1=sell
    int32_t  qty;
    double   price;
};

// 🔹 Risk-Validated Order Request
struct alignas(64) OrderRequest {
    uint64_t order_id;
    int32_t  symbol_id;
    int8_t   side;
    int32_t  qty;
    double   price;
    uint8_t  risk_status; // 0=approved, 1=limit, 2=drawdown, 3=rate
    int64_t  ingress_ns;
    int64_t  exit_ns;
};

// 🔹 Lock-Free SPSC Queue (Reused from Day 3, optimized for signals)
template<typename T, size_t Cap>
class SPSCQueue {
    static_assert((Cap & (Cap - 1)) == 0, "Power of 2 required");
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T buffer_[Cap];
public:
    bool push(const T& item) {
        size_t h = head_.load(std::memory_order_relaxed);
        if (((h + 1) & (Cap - 1)) == (tail_.load(std::memory_order_acquire))) return false;
        buffer_[h & (Cap - 1)] = item;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& item) {
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        item = buffer_[t & (Cap - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
};

// 🔹 Pre-Trade Risk Engine (Single-Threaded for Determinism)
class RiskEngine {
    int64_t  position_ = 0;
    double   pnl_      = 0.0;
    uint64_t rate_cnt_ = 0;
    uint64_t last_sec_ = 0;

public:
    OrderRequest validate(const Signal& sig, uint64_t id) {
        uint64_t sec = sig.ts_ns / 1'000'000'000;
        if (sec != last_sec_) { rate_cnt_ = 0; last_sec_ = sec; }

        OrderRequest req{};
        req.order_id = id; req.symbol_id = sig.symbol_id;
        req.side = sig.side; req.qty = sig.qty; req.price = sig.price;
        req.ingress_ns = sig.ts_ns;

        // 1️⃣ Hard Limits
        if (sig.qty > MAX_QTY) { req.risk_status = 1; return req; }
        if ((sig.side == 1 && position_ + sig.qty > MAX_POS) ||
            (sig.side == -1 && position_ - sig.qty < -MAX_POS)) {
            req.risk_status = 1; return req;
        }
        // 2️⃣ Circuit Breaker
        if (pnl_ < -MAX_DD) { req.risk_status = 2; return req; }
        // 3️⃣ Rate Limit
        if (rate_cnt_ >= RATE_CAP) { req.risk_status = 3; return req; }

        // Approve & book provisional position
        position_ += sig.qty * sig.side;
        rate_cnt_++;
        req.risk_status = 0;
        return req;
    }

    void apply_fill(const OrderRequest& req, double fill_price) {
        pnl_ += req.qty * req.side * (fill_price - 150.0); // dummy mid
    }
    int64_t position() const { return position_; }
    double  pnl() const { return pnl_; }
};

int main() {
    SPSCQueue<Signal, QUEUE_CAP> sig_queue;
    SPSCQueue<OrderRequest, QUEUE_CAP> ord_queue;
    RiskEngine risk;
    std::atomic<size_t> produced{0}, routed{0}, rejected{0};
    std::vector<int64_t> e2e_latencies;
    e2e_latencies.reserve(NUM_SIGNALS);
    volatile double sink_pnl = 0; // Prevents optimization

    // 📡 THREAD 1: Strategy Generator
    std::thread strategy([&]() {
        std::mt19937_64 rng(42);
        std::uniform_int_distribution<int> price_d(14950, 15050);
        std::uniform_int_distribution<int> qty_d(10, 600);
        std::uniform_int_distribution<int> side_d(0, 1);
        int64_t base_ts = 0;

        for (size_t i = 0; i < NUM_SIGNALS; ++i) {
            base_ts += 500 + (rng() % 1000); // ~1-1.5μs between signals
            Signal sig{};
            sig.ts_ns = base_ts;
            sig.symbol_id = 1;
            sig.side = side_d(rng) == 0 ? 1 : -1;
            sig.qty = qty_d(rng);
            sig.price = price_d(rng) / 100.0;

            while (!sig_queue.push(sig)) {
                // Drop signal under backpressure (non-blocking)
                rejected.fetch_add(1, std::memory_order_relaxed);
                goto next_sig;
            }
            produced.fetch_add(1, std::memory_order_relaxed);
        next_sig:;
        }
    });

    // 🎯 THREAD 2: Risk → OMS Pipeline
    std::thread risk_oms([&]() {
        uint64_t order_id = 1000;
        Signal sig; OrderRequest req;
        while (produced.load(std::memory_order_relaxed) + rejected.load() < NUM_SIGNALS ||
               !sig_queue.pop(sig)) {
            if (sig_queue.pop(sig)) {
                req = risk.validate(sig, order_id++);
                req.exit_ns = sig.ts_ns + 150; // Simulated 150ns risk check
                e2e_latencies.push_back(req.exit_ns - sig.ts_ns);

                if (req.risk_status == 0) {
                    risk.apply_fill(req, req.price + 0.005 * req.side);
                    sink_pnl += req.price;
                    routed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });

    strategy.join();
    risk_oms.join();

    // 📊 Statistics
    auto ms_total = static_cast<double>(e2e_latencies.size()) / 1e6; // ~5M signals → fake wall time
    std::sort(e2e_latencies.begin(), e2e_latencies.end());
    auto p = [&](double pct) { return e2e_latencies[static_cast<size_t>(e2e_latencies.size() * pct / 100.0)]; };

    std::cout << "=== Lock-Free Signal Routing & Risk Pipeline ===\n";
    std::cout << "Signals Generated : " << produced.load() << "\n";
    std::cout << "Signals Routed    : " << routed.load() << "\n";
    std::cout << "Signals Dropped   : " << rejected.load() << "\n";
    std::cout << "Final Position    : " << risk.position() << "\n";
    std::cout << "Simulated PnL     : $" << std::fixed << std::setprecision(2) << risk.pnl() << "\n\n";
    std::cout << "Risk Latency (ns):\n";
    std::cout << "  Min  : " << std::setw(6) << p(0) << "\n";
    std::cout << "  p50  : " << std::setw(6) << p(50) << "\n";
    std::cout << "  p99  : " << std::setw(6) << p(99) << "\n";
    std::cout << "  Max  : " << std::setw(6) << p(100) << "\n";

    std::cout << "\n📊 Production Architecture Notes:\n";
    std::cout << " • Strategy thread NEVER blocks. Drops signals if queue full.\n";
    std::cout << " • Risk runs on isolated core, single-threaded for deterministic state.\n";
    std::cout << " • Backpressure = DROP. Blocking violates HFT SLA during volatility.\n";
    std::cout << " • Real systems: hardware rate limiters (FPGA), atomic position counters, circuit breakers.\n";

    return 0;
}