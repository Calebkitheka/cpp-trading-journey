#include <iostream>
#include <vector>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <algorithm>

constexpr size_t NUM_EVENTS = 2'000'000;
constexpr size_t RING_SIZE = 1 << 16; // 65,536 (fits in L2 cache)

struct alignas(64) MarketEvent {
    uint64_t timestamp_ns;
    uint32_t payload_len;
    uint8_t  data[48];
};

// 🔹 Simulated Kernel-Bypass Ring Buffer (DPDK/Solarflare style)
class KBRing {
    alignas(64) std::atomic<uint64_t> head_{0}, tail_{0};
    MarketEvent buffer_[RING_SIZE];
public:
    bool push(const MarketEvent& e) {
        uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= RING_SIZE) return false;
        buffer_[h & (RING_SIZE - 1)] = e;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(MarketEvent& e) {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        e = buffer_[t & (RING_SIZE - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
};

// Simulated OS overheads
inline void simulate_standard_syscall() {
    volatile int sink = 0;
    for (int i = 0; i < 120; ++i) sink += i; // Context switch + buffer copy + Nagle delay
}
inline void simulate_tuned_path() {
    volatile int sink = 0;
    for (int i = 0; i < 25; ++i) sink += i; // TCP_NODELAY + larger buffers + optimized vDSO
}
inline void simulate_kernel_bypass() {
    // Zero overhead: direct memory access, no syscalls, no kernel context switch
}

template<typename OverheadFn>
double benchmark(const char* name, OverheadFn overhead_fn, KBRing* ring = nullptr) {
    MarketEvent evt{123456789, 32, {0}};
    std::memset(evt.data, 0xAA, sizeof(evt.data));
    volatile uint64_t sink = 0;

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < NUM_EVENTS; ++i) {
        if (ring) {
            // Push (simulates NIC DMA write)
            while (!ring->push(evt)) std::this_thread::yield();
            // Pop (simulates EPOLLET / poll loop)
            if (ring->pop(evt)) sink += evt.timestamp_ns;
        }
        overhead_fn();
    }

    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

int main() {
    std::cout << "=== Network Stack Tuning & Kernel-Bypass Simulation ===\n";
    std::cout << "Events: " << NUM_EVENTS << "\n\n";

    KBRing ring;

    double ms_std = benchmark("Standard Stack (Default)", []{ simulate_standard_syscall(); });
    double ms_tuned = benchmark("Tuned Stack (NODELAY + Buffers)", []{ simulate_tuned_path(); });
    double ms_kb  = benchmark("Simulated Kernel-Bypass", []{ simulate_kernel_bypass(); }, &ring);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << std::left << std::setw(35) << "Standard (Nagle + LT + Syscalls)" << ": " << ms_std << " ms\n";
    std::cout << std::left << std::setw(35) << "Tuned (TCP_NODELAY + Optimized)" << ": " << ms_tuned << " ms\n";
    std::cout << std::left << std::setw(35) << "Kernel-Bypass (Ring Buffer + ET)" << ": " << ms_kb << " ms\n";
    std::cout << "\nSpeedup (Std → KB) : " << (ms_std / std::max(0.1, ms_kb)) << "x\n";
    std::cout << "Speedup (Tuned → KB): " << (ms_tuned / std::max(0.1, ms_kb)) << "x\n\n";

    std::cout << "📊 Real-World HFT Network Tuning:\n";
    std::cout << "Linux:\n";
    std::cout << "  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));\n";
    std::cout << "  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &large_buf, sizeof(large_buf));\n";
    std::cout << "  epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &(struct epoll_event){.events = EPOLLIN | EPOLLET});\n";
    std::cout << "\nWindows (IOCP/Winsock):\n";
    std::cout << "  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&on, sizeof(on));\n";
    std::cout << "  WSARecv + WSABUF scatter-gather for zero-copy buffer chaining\n";
    std::cout << "  CreateIoCompletionPort + GetQueuedCompletionStatusEx for ET-style polling\n\n";
    std::cout << "🔑 Key Rule: In HFT, every syscall is a latency penalty. Kernel-bypass eliminates them.\n";

    return 0;
}