#include <iostream>
#include <vector>
#include <cmath>
#include <iomanip>
#include <random>
#include <cstdint>

struct Tick { double price; double volume; uint64_t ts_ms; };
enum class SignalType { BUY, SELL, HOLD };
struct AlphaSignal { SignalType type; double strength; uint64_t ts_ms; };

// 🔹 Incremental EMA (O(1) update, no array scan)
class EMA {
    double val_ = 0.0;
    bool init_ = false;
    double alpha_;
public:
    EMA(double period) : alpha_(2.0 / (period + 1.0)) {}
    double update(double price) {
        if (!init_) { val_ = price; init_ = true; }
        else val_ = alpha_ * price + (1.0 - alpha_) * val_;
        return val_;
    }
    double value() const { return val_; }
};

// 🔹 Rolling Statistics with Fixed-Size Ring Buffer (Cache-Friendly)
class RollingStats {
    std::vector<double> buffer_;
    size_t capacity_, head_ = 0, count_ = 0;
    double sum_ = 0.0, sum_sq_ = 0.0;
public:
    RollingStats(size_t n) : capacity_(n), buffer_(n) {}
    
    double update(double val) {
        if (count_ == capacity_) {
            double old = buffer_[head_];
            sum_ -= old;
            sum_sq_ -= old * old;
        } else {
            ++count_;
        }
        buffer_[head_] = val;
        head_ = (head_ + 1) % capacity_;
        sum_ += val;
        sum_sq_ += val * val;
        return std::sqrt(std::max(0.0, (sum_sq_ / count_) - (mean() * mean())));
    }
    double mean() const { return count_ ? sum_ / count_ : 0.0; }
    double stddev() const {
        if (count_ < 2) return 0.0;
        double m = mean();
        return std::sqrt(std::max(0.0, (sum_sq_ / count_) - (m * m)));
    }
};

// 🔹 VWAP (Volume Weighted Average Price)
class VWAP {
    double cum_vol_price_ = 0.0, cum_vol_ = 0.0;
public:
    double update(double price, double vol) {
        cum_vol_price_ += price * vol;
        cum_vol_ += vol;
        return cum_vol_ == 0 ? price : cum_vol_price_ / cum_vol_;
    }
    void daily_reset() { cum_vol_price_ = 0.0; cum_vol_ = 0.0; }
};

// 🔹 Alpha Signal Generator (Mean-Reversion + Trend Filter)
class AlphaGenerator {
    EMA fast_ema, slow_ema;
    VWAP vwap;
    RollingStats bb_std;
    double bb_mult_ = 2.0;
public:
    AlphaGenerator(int fast, int slow, int bb_window) 
        : fast_ema(fast), slow_ema(slow), bb_std(bb_window) {}

    AlphaSignal process(const Tick& t) {
        double f = fast_ema.update(t.price);
        double s = slow_ema.update(t.price);
        double v = vwap.update(t.price, t.volume);
        double std = bb_std.update(t.price);
        double bb_mean = bb_std.mean();

        // Z-Score quantifies how many std devs price is from rolling mean
        double z = (std > 1e-6) ? (t.price - bb_mean) / std : 0.0;
        bool trend_up = f > s;

        SignalType type = SignalType::HOLD;
        double strength = 0.0;

        // Regime Logic: Only trade mean-reversion when aligned with trend
        if (trend_up && z < -bb_mult_) {
            type = SignalType::BUY;
            strength = std::min(1.0, std::abs(z + bb_mult_) / bb_mult_);
        } else if (!trend_up && z > bb_mult_) {
            type = SignalType::SELL;
            strength = std::min(1.0, std::abs(z - bb_mult_) / bb_mult_);
        }

        return {type, strength, t.ts_ms};
    }
};

int main() {
    AlphaGenerator alpha(10, 50, 20); // Fast EMA, Slow EMA, BB Window
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> noise(-0.5, 0.5);
    std::uniform_real_distribution<double> vol_dist(100.0, 800.0);

    double price = 150.0;
    std::vector<AlphaSignal> signals;
    int buys = 0, sells = 0;

    std::cout << "=== Alpha Signal Generation & Feature Engineering ===\n";
    std::cout << "Simulating 50,000 ticks with regime-aware signals...\n\n";

    for (int i = 0; i < 50'000; ++i) {
        price += noise(rng) * 0.1; // Random walk with micro-drift
        Tick t{price, vol_dist(rng), static_cast<uint64_t>(i * 5)};
        auto sig = alpha.process(t);
        
        if (sig.type == SignalType::BUY) buys++;
        else if (sig.type == SignalType::SELL) sells++;
        
        if (sig.type != SignalType::HOLD) signals.push_back(sig);
    }

    double avg_buy_str = 0, avg_sell_str = 0;
    for (auto& s : signals) {
        if (s.type == SignalType::BUY) avg_buy_str += s.strength;
        else avg_sell_str += s.strength;
    }
    avg_buy_str = buys ? avg_buy_str / buys : 0.0;
    avg_sell_str = sells ? avg_sell_str / sells : 0.0;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total Signals   : " << signals.size() << "\n";
    std::cout << "Buy Signals     : " << buys << " (Avg Strength: " << avg_buy_str << ")\n";
    std::cout << "Sell Signals    : " << sells << " (Avg Strength: " << avg_sell_str << ")\n";
    std::cout << "Signal Density  : " << (100.0 * signals.size() / 50000.0) << "%\n\n";

    std::cout << "📊 Feature Engineering Principles:\n";
    std::cout << " • Incremental updates > full-window recalculation (O(1) vs O(N))\n";
    std::cout << " • VWAP anchors to institutional fair value & VWAP execution algos\n";
    std::cout << " • EMA crossovers filter noise & define trend regime\n";
    std::cout << " • Z-score + BB quantifies mean-reversion edges with statistical rigor\n";
    std::cout << " • Production systems: Welford's algo for variance, decay-weighted VWAP, cross-asset features\n";

    return 0;
}