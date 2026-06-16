#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <random>
#include <iomanip>
#include <algorithm>
#include <cmath>

// 🔹 Constants for the Model
constexpr double INITIAL_CASH = 1'000'000.0;
constexpr double FEE_PER_TRADE = 0.001; // 0.1% maker/taker fee
constexpr double SPREAD_LAG = 5;       // Ticks latency for fill

struct Position {
    int64_t quantity = 0;
    double entry_price = 0.0;
};

// 🔹 Simple Market Data Event (Simulating Day 18 Replay Engine Output)
struct MarketEvent {
    uint64_t timestamp_ms;
    double ask_price;
    double bid_price;
    int64_t volume;
};

// 🔹 Order State Machine
enum class OrderStatus { 
    PENDING,      // Just created
    QUEUED,       // Waiting for latency/slippage window
    FILLED,       // Executed
    REJECTED      // Blocked by Risk (from Day 19)
};

struct Order {
    int64_t id;
    double requested_price;
    int64_t qty;
    int8_t side; // 1=Buy, -1=Sell
    uint64_t creation_time_ms;
    OrderStatus status;
    double fill_price = 0.0;
};

// 🔹 The Backtest Orchestrator
class BacktestEngine {
    double cash_ = INITIAL_CASH;
    Position pos_;
    std::vector<Order> pending_orders_;
    std::vector<MarketEvent> history_;
    
    // Metrics
    double max_drawdown_ = 0.0;
    double peak_equity_ = INITIAL_CASH;
    double unrealized_pnl_ = 0.0;
    double total_fees_ = 0.0;

public:
    // 1️⃣ Process one tick (Market Move)
    void process_tick(MarketEvent evt) {
        history_.push_back(evt);
        
        // Update Unrealized PnL
        double mid = (evt.ask_price + evt.bid_price) / 2.0;
        unrealized_pnl_ = static_cast<double>(pos_.quantity) * (mid - pos_.entry_price);

        // Update Equity Tracking
        double equity = cash_ + unrealized_pnl_ - total_fees_;
        if (equity > peak_equity_) peak_equity_ = equity;
        else max_drawdown_ = std::max(max_drawdown_, peak_equity_ - equity);

        // 2️⃣ Match Pending Orders (Execution Simulation)
        execute_pending(evt.timestamp_ms, mid);
    }

    // 3️⃣ Submit Signal -> Risk -> Pending Queue
    bool submit_signal(double price, int64_t qty, int8_t side, uint64_t ms) {
        // --- Risk Gate (Mocking Day 19 Logic) ---
        if (cash_ < price * qty) return false; // Margin fail
        
        Order o;
        o.id = history_.size();
        o.requested_price = price;
        o.qty = qty;
        o.side = side;
        o.creation_time_ms = ms;
        o.status = OrderStatus::PENDING;
        pending_orders_.push_back(o);
        return true;
    }

    // 4️⃣ Execution Logic (Latency Drag & Slippage)
    void execute_pending(uint64_t current_ms, double mid_price) {
        for (auto it = pending_orders_.begin(); it != pending_orders_.end();) {
            Order* o = &(*it);
            
            // Check Latency/Drag: Can't fill until N timesteps later
            if (current_ms - o->creation_time_ms >= SPREAD_LAG) {
                // Check Slippage: Did price cross our threshold?
                bool crossed = (o->side == 1 && mid_price <= o->requested_price) ||
                               (o->side == -1 && mid_price >= o->requested_price);
                
                if (crossed) {
                    // Execute! (Slippage modeled as filling slightly worse than limit)
                    double exec_price = (o->side == 1) ? o->requested_price + 0.02 : o->requested_price - 0.02;
                    
                    finish_fill(o, exec_price);
                    it = pending_orders_.erase(it);
                } else {
                    ++it;
                }
            } else {
                ++it;
            }
        }
    }

    void finish_fill(Order* o, double fill_price) {
        double cost = fill_price * o->qty;
        
        if (o->side == 1) {
            // Buy
            cash_ -= (cost + cost * FEE_PER_TRADE);
            if (pos_.quantity == 0) pos_.entry_price = fill_price;
            pos_.quantity += o->qty;
            total_fees_ += cost * FEE_PER_TRADE;
        } else {
            // Sell
            cash_ += (cost - cost * FEE_PER_TRADE);
            pos_.quantity -= o->qty;
            if (pos_.quantity == 0) pos_.entry_price = 0.0; // Reset avg entry
            total_fees_ += cost * FEE_PER_TRADE;
        }
        o->status = OrderStatus::FILLED;
        o->fill_price = fill_price;
    }

    // Report Card
    void print_report() const {
        double final_equity = cash_ + unrealized_pnl_ - total_fees_;
        double total_return_pct = ((final_equity - INITIAL_CASH) / INITIAL_CASH) * 100;
        double dd_pct = (max_drawdown_ / peak_equity_) * 100;

        std::cout << "=== BACKTEST PERFORMANCE REPORT ===\n";
        std::cout << "Initial Capital : $" << INITIAL_CASH << "\n";
        std::cout << "Final Equity    : $" << std::fixed << std::setprecision(2) << final_equity << "\n";
        std::cout << "Total Return    : " << total_return_pct << "%\n";
        std::cout << "Max Drawdown    : " << dd_pct << "%\n";
        std::cout << "Trading Fees    : $" << total_fees_ << "\n\n";
        std::cout << "📉 Key Takeaway: In HFT, fees are the killer.\n";
        std::cout << "   Your edge must exceed fee drag + latency slipper.\n";
    }
};

int main() {
    BacktestEngine bt;
    std::mt19937 rng(42);

    double price = 150.0;
    std::cout << "Starting Backtest Loop...\n\n";

    // Simulate 1 year of minute-bars
    for (int i = 0; i < 1000; ++i) {
        // Random Walk
        price += (rng() % 100 - 50) * 0.01; 
        
        MarketEvent evt{i, price + 0.05, price - 0.05, 10000};
        bt.process_tick(evt);

        // Simple Strategy: Mean Reversion (Randomly implemented for demo)
        // If price drops significantly, buy
        if (i > 50 && i % 5 == 0) {
            // Generate random signal
            if (rng() % 10 < 3) {
                bt.submit_signal(price, 100, 1, i); // Limit Buy
            } else {
                bt.submit_signal(price, 50, -1, i); // Limit Sell
            }
        }
    }

    bt.print_report();
    return 0;
}