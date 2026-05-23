#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <numeric>
#include <algorithm>

int main() {
    constexpr size_t N = 10'000'000; // ~40MB (exceeds typical L3 cache)
    std::vector<int> data(N);
    
    // Fill with sequential values
    std::iota(data.begin(), data.end(), 1);

    // 1️⃣ SEQUENTIAL ACCESS (Cache-Friendly)
    auto start_seq = std::chrono::high_resolution_clock::now();
    long long sum_seq = 0;
    for (size_t i = 0; i < N; ++i) {
        sum_seq += data[i];
    }
    auto end_seq = std::chrono::high_resolution_clock::now();
    auto ms_seq = std::chrono::duration_cast<std::chrono::milliseconds>(end_seq - start_seq).count();

    // 2️⃣ RANDOM ACCESS (Cache-Unfriendly)
    std::vector<size_t> indices(N);
    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937_64 rng(42); // Fixed seed for reproducible results
    std::shuffle(indices.begin(), indices.end(), rng);

    auto start_rand = std::chrono::high_resolution_clock::now();
    long long sum_rand = 0;
    for (size_t idx : indices) {
        sum_rand += data[idx];
    }
    auto end_rand = std::chrono::high_resolution_clock::now();
    auto ms_rand = std::chrono::duration_cast<std::chrono::milliseconds>(end_rand - start_rand).count();

    // Print results (prevents compiler from optimizing loops away)
    std::cout << "=== CPU Cache Locality Benchmark ===\n";
    std::cout << "Dataset: " << N << " ints (~" << (N * 4) / (1024.0 * 1024.0) << " MB)\n";
    std::cout << "Sequential: " << ms_seq << " ms\n";
    std::cout << "Random:     " << ms_rand << " ms\n";
    std::cout << "Speedup:    " << (double)ms_rand / ms_seq << "x faster\n";
    std::cout << "Sums match: " << (sum_seq == sum_rand ? "✅" : "❌") << "\n";

    return 0;
}