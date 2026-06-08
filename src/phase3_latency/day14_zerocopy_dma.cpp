#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <array>
#include <iomanip>

constexpr size_t NUM_PACKETS = 2'000'000;
constexpr size_t MAX_PAYLOAD = 128;
constexpr size_t SLAB_COUNT  = NUM_PACKETS;

// 🔹 Slab Allocator for parsed message metadata (O(1), zero fragmentation)
class MessageSlab {
    alignas(64) std::array<std::byte, sizeof(uint64_t) * SLAB_COUNT> memory_;
    std::array<size_t, SLAB_COUNT> next_idx_;
    size_t head_ = 0;
public:
    MessageSlab() {
        for (size_t i = 0; i < SLAB_COUNT - 1; ++i) next_idx_[i] = i + 1;
        next_idx_[SLAB_COUNT - 1] = SLAB_COUNT;
    }
    uint64_t* allocate() {
        if (head_ == SLAB_COUNT) return nullptr;
        size_t idx = head_;
        head_ = next_idx_[idx];
        return reinterpret_cast<uint64_t*>(memory_.data() + idx * sizeof(uint64_t));
    }
    void deallocate(uint64_t* ptr) {
        size_t idx = (reinterpret_cast<std::byte*>(ptr) - memory_.data()) / sizeof(uint64_t);
        next_idx_[idx] = head_;
        head_ = idx;
    }
};

// 📦 Simulated NIC Packet (Header + Variable Payload)
struct alignas(64) NicPacket {
    uint64_t timestamp_ns;
    uint16_t payload_len;
    uint8_t  msg_type; // 1=ADD, 2=EXEC, 3=CANCEL
    uint8_t  _pad[5];
    uint8_t  payload[MAX_PAYLOAD];
};

// ❌ TRADITIONAL: Copy to heap, dynamic allocation, parse later
struct ParsedMsgHeap {
    uint64_t ts;
    uint16_t len;
    uint8_t type;
    std::vector<uint8_t> data; // malloc per message!
};

size_t benchmark_traditional(const std::vector<NicPacket>& packets) {
    std::vector<ParsedMsgHeap> parsed;
    parsed.reserve(packets.size());
    size_t checksum = 0;
    for (const auto& pkt : packets) {
        ParsedMsgHeap msg;
        msg.ts = pkt.timestamp_ns;
        msg.len = pkt.payload_len;
        msg.type = pkt.msg_type;
        msg.data.resize(pkt.payload_len); // Heap allocation
        std::memcpy(msg.data.data(), pkt.payload, pkt.payload_len);
        checksum += msg.type * msg.len;
        parsed.push_back(std::move(msg));
    }
    return checksum;
}

// ✅ ZERO-COPY: Direct pointer overlay + slab metadata allocation
struct ZeroCopyMsg {
    uint64_t timestamp_ns;
    uint16_t payload_len;
    uint8_t  msg_type;
    const uint8_t* payload_ptr; // Points DIRECTLY into DMA buffer
};

size_t benchmark_zerocopy(const std::vector<NicPacket>& packets, MessageSlab& slab) {
    std::vector<ZeroCopyMsg*> parsed;
    parsed.reserve(packets.size());
    size_t checksum = 0;
    for (const auto& pkt : packets) {
        auto* meta = slab.allocate();
        auto* msg = reinterpret_cast<ZeroCopyMsg*>(meta);
        msg->timestamp_ns = pkt.timestamp_ns;
        msg->payload_len  = pkt.payload_len;
        msg->msg_type     = pkt.msg_type;
        msg->payload_ptr  = pkt.payload; // Zero-copy pointer
        checksum += msg->msg_type * msg->payload_len;
        parsed.push_back(msg);
    }
    return checksum;
}

// Simulate NIC writing variable-length packets into user-space buffer
std::vector<NicPacket> generate_nic_traffic(size_t count) {
    std::vector<NicPacket> buf;
    buf.reserve(count);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> len_dist(10, MAX_PAYLOAD);
    std::uniform_int_distribution<int> type_dist(1, 3);

    for (size_t i = 0; i < count; ++i) {
        NicPacket pkt{};
        pkt.timestamp_ns = i * 500; // ~2μs between ticks
        pkt.payload_len  = static_cast<uint16_t>(len_dist(rng));
        pkt.msg_type     = static_cast<uint8_t>(type_dist(rng));
        std::memset(pkt.payload, 0xAA, pkt.payload_len); // Dummy market data
        buf.push_back(pkt);
    }
    return buf;
}

int main() {
    std::cout << "=== Zero-Copy DMA vs Traditional Heap Parsing ===\n";
    std::cout << "Packets: " << NUM_PACKETS << "\n";
    std::cout << "Simulating: Kernel-Bypass NIC (DPDK/Solarflare)\n\n";

    auto traffic = generate_nic_traffic(NUM_PACKETS);
    MessageSlab slab;

    // Traditional
    auto start_t = std::chrono::high_resolution_clock::now();
    volatile size_t ck_t = benchmark_traditional(traffic);
    auto end_t = std::chrono::high_resolution_clock::now();
    auto ms_t = std::chrono::duration_cast<std::chrono::milliseconds>(end_t - start_t).count();

    // Zero-Copy
    auto start_z = std::chrono::high_resolution_clock::now();
    volatile size_t ck_z = benchmark_zerocopy(traffic, slab);
    auto end_z = std::chrono::high_resolution_clock::now();
    auto ms_z = std::chrono::duration_cast<std::chrono::milliseconds>(end_z - start_z).count();

    std::cout << std::fixed << std::setprecision(2);
    std::cout << std::left << std::setw(22) << "Traditional (memcpy+heap): " << ms_t << " ms\n";
    std::cout << std::left << std::setw(22) << "Zero-Copy (slab+ptr)    : " << ms_z << " ms\n";
    std::cout << "Speedup               : " << (ms_t / std::max(0.1, ms_z)) << "x\n";
    std::cout << "Checksum Match        : " << (ck_t == ck_z ? "✅" : "❌") << "\n\n";

    std::cout << "📊 Why Zero-Copy Wins in Colocation:\n";
    std::cout << " • Eliminates malloc/free → no heap fragmentation or lock contention\n";
    std::cout << " • Removes memcpy → CPU cache stays hot, avoids bandwidth saturation\n";
    std::cout << " • Pointer overlay → parsing happens during NIC DMA write (sub-μs)\n";
    std::cout << " • Production: DPDK mbufs, Solarflare OpenOnload, ExaNIC direct mapping\n";

    return 0;
}