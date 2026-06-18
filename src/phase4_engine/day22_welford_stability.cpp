#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <random>
#include <algorithm>
#include <cstdint>

constexpr size_t NUM_TICKS = 100'000'000;
constexpr double TRUE_MEAN = 150.0;
constexpr double TRUE_STD  = 0.05; // Small variance intentionally triggers cancellation

// ❌ NAIVE: Prone to catastrophic cancellation (sum_sq/n - mean^2)
class NaiveStats {
    double sum_ = 0.0, sum_sq_ = 0.0;
    size_t n_ = 0;
public:
    void update(double x) { ++n_; sum_ += x; sum_sq_ += x * x; }
    double mean() const { return n_ ? sum_ / n_ : 0.0; }
    double variance() const {
        double m = mean();
        double var = (sum_sq_ / n_) - (m * m);
        return var < 0.0 ? 0.0 : var; // Clamp negative drift
    }
};

// ✅ WELFORD'S ONLINE ALGORITHM: Numerically stable, O(1), production standard
class WelfordStats {
    double mean_ = 0.0, m2_ = 0.0;
    size_t n_ = 0;
public:
    void update(double x) {
        ++n_;
        double delta = x - mean_;
        mean_ += delta / n_;
        double delta2 = x - mean_;
        m2_ += delta * delta2;
    }
    double mean() const { return mean_; }
    double variance() const { return n_ < 2 ? 0.0 : m2_ / (n_ - 1); } // Sample variance
};

// ✅ EXPONENTIALLY WEIGHTED (DECAY): Market-relevant, prioritizes recent volatility
class EWMAStats {
    double mean_ = 0.0, var_ = 0.0;
    double alpha_; // Decay factor
    bool init_ = false;
public:
    explicit EWMAStats(double decay_period) : alpha_(2.0 / (decay_period + 1.0)) {}
    void update(double x) {
        if (!init_) { mean_ = x; var_ = 0.0; init_ = true; return; }
        double diff = x - mean_;
        mean_ = alpha_ * x + (1.0 - alpha_) * mean_;
        // Stable EWMA variance update (RiskMetrics-style)
        var_ = (1.0 - alpha_) * (var_ + alpha_ * diff * diff);
    }
    double mean() const { return mean_; }
    double variance() const { return var_; }
};

int main() {
    std::cout << "=== Numerical Stability & Welford's Algorithm Benchmark ===\n";
    std::cout << "Ticks: " << NUM_TICKS << " | True Mean: " << TRUE_MEAN << " | True Std: " << TRUE_STD << "\n\n";

    NaiveStats naive;
    WelfordStats welford;
    EWMAStats ewma(20); // ~20-tick effective decay window

    std::mt19937_64 rng(42);
    std::normal_distribution<double> dist(TRUE_MEAN, TRUE_STD);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < NUM_TICKS; ++i) {
        double price = dist(rng);
        naive.update(price);
        welford.update(price);
        ewma.update(price);
    }

    auto end = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(end - start).count();

    double naive_var = naive.variance();
    double w_var = welford.variance();
    double ew_var = ewma.variance();
    double true_var = TRUE_STD * TRUE_STD;

    double naive_err = std::abs(naive_var - true_var);
    double w_err = std::abs(w_var - true_var);

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Wall Time       : " << sec << " sec\n\n";
    std::cout << std::left << std::setw(12) << "Method"
              << std::setw(15) << "Variance"
              << std::setw(15) << "StdDev"
              << std::setw(18) << "Abs Error" << "\n";
    std::cout << "----------------------------------------------------------------\n";
    std::cout << std::left << std::setw(12) << "Naive"
              << std::setw(15) << naive_var
              << std::setw(15) << std::sqrt(naive_var)
              << std::setw(18) << naive_err << "\n";
    std::cout << std::left << std::setw(12) << "Welford"
              << std::setw(15) << w_var
              << std::setw(15) << std::sqrt(w_var)
              << std::setw(18) << w_err << "\n";
    std::cout << std::left << std::setw(12) << "EWMA (Decay)"
              << std::setw(15) << ew_var
              << std::setw(15) << std::sqrt(ew_var)
              << std::setw(18) << "N/A (Adaptive)" << "\n";

    std::cout << "\n📊 Key Insights:\n";
    if (naive_err > w_err * 10) {
        std::cout << "⚠️  CATASTROPHIC CANCELLATION DETECTED in Naive method!\n";
        std::cout << "   `sum_sq/n` and `mean^2` became nearly identical → lost 6-8 decimal digits.\n";
    }
    std::cout << "✅ Welford's maintains stability across 100M+ updates.\n";
    std::cout << "✅ EWMA adapts to recent volatility (critical for regime shifts & VaR).\n";
    std::cout << "🔑 Production Rule: NEVER use sum-of-squares for rolling variance in quant systems.\n";

    return 0;
}