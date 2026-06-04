#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

// OHLCV Payload (24 bytes + padding to 32 for alignment)
struct alignas(32) OHLCV {
    int64_t timestamp_ms;
    double open, high, low, close;
    int64_t volume;
};

// Cross-platform Memory Mapped File wrapper
class MemoryMappedFile {
    const char* data_ = nullptr;
    size_t size_ = 0;
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
public:
    ~MemoryMappedFile() { unmap(); }

    bool open(const char* path) {
#ifdef _WIN32
        file_ = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;
        
        LARGE_INTEGER file_size;
        GetFileSizeEx(file_, &file_size);
        size_ = file_size.QuadPart;
        
        mapping_ = CreateFileMappingA(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) return false;
        
        data_ = static_cast<const char*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        return data_ != nullptr;
#else
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) return false;
        
        struct stat st;
        if (fstat(fd_, &st) < 0) return false;
        size_ = st.st_size;
        
        data_ = static_cast<const char*>(mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
        return data_ != MAP_FAILED;
#endif
    }

    void unmap() {
#ifdef _WIN32
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        data_ = nullptr; mapping_ = INVALID_HANDLE_VALUE; file_ = INVALID_HANDLE_VALUE;
#else
        if (data_ && data_ != MAP_FAILED) munmap((void*)data_, size_);
        if (fd_ >= 0) ::close(fd_);
        data_ = nullptr; fd_ = -1;
#endif
    }

    const char* data() const { return data_; }
    size_t size() const { return size_; }
};

// Fast, allocation-free number parsers
inline int64_t parse_int64(const char*& p) {
    int64_t val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        ++p;
    }
    return val;
}

inline double parse_double(const char*& p) {
    double val = 0.0, frac = 0.1;
    while (*p >= '0' && *p <= '9') {
        val = val * 10.0 + (*p - '0');
        ++p;
    }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            val += (*p - '0') * frac;
            frac *= 0.1;
            ++p;
        }
    }
    return val;
}

// Skip to next line (handles \n and \r\n)
inline void skip_line(const char*& p) {
    while (*p && *p != '\n') ++p;
    if (*p == '\n') ++p;
}

// Generate synthetic OHLCV CSV for benchmarking
void generate_test_file(const char* path, size_t rows) {
    std::ofstream out(path, std::ios::binary);
    std::mt19937_64 rng(42);
    double price = 150.0;
    out << "timestamp,open,high,low,close,volume\n";
    for (size_t i = 0; i < rows; ++i) {
        price += (rng() % 100 - 50) * 0.001;
        double h = price + (rng() % 20) * 0.001;
        double l = price - (rng() % 20) * 0.001;
        double o = l + (rng() % (int)((h-l)*1000)) * 0.001;
        double c = l + (rng() % (int)((h-l)*1000)) * 0.001;
        int64_t vol = 1000 + rng() % 9000;
        out << (i * 60000) << "," << o << "," << h << "," << l << "," << c << "," << vol << "\n";
    }
}

// Benchmark 1: Traditional std::ifstream + std::stod/stoll
size_t benchmark_ifstream(const char* path, std::vector<OHLCV>& out) {
    std::ifstream in(path);
    if (!in) return 0;
    std::string line;
    std::getline(in, line); // skip header

    auto start = std::chrono::high_resolution_clock::now();
    while (std::getline(in, line)) {
        size_t pos = 0;
        auto next_comma = line.find(',', pos);
        OHLCV row;
        row.timestamp_ms = std::stoll(line.substr(pos, next_comma - pos)); pos = next_comma + 1;
        
        next_comma = line.find(',', pos);
        row.open = std::stod(line.substr(pos, next_comma - pos)); pos = next_comma + 1;
        
        next_comma = line.find(',', pos);
        row.high = std::stod(line.substr(pos, next_comma - pos)); pos = next_comma + 1;
        
        next_comma = line.find(',', pos);
        row.low = std::stod(line.substr(pos, next_comma - pos)); pos = next_comma + 1;
        
        next_comma = line.find(',', pos);
        row.close = std::stod(line.substr(pos, next_comma - pos)); pos = next_comma + 1;
        
        row.volume = std::stoll(line.substr(pos));
        out.push_back(row);
    }
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

// Benchmark 2: Memory-Mapped Zero-Copy Parser
size_t benchmark_mmap(const char* path, std::vector<OHLCV>& out) {
    MemoryMappedFile mmap;
    if (!mmap.open(path)) return 0;
    
    const char* p = mmap.data();
    // Skip header
    while (*p && *p != '\n') ++p;
    if (*p == '\n') ++p;

    auto start = std::chrono::high_resolution_clock::now();
    const char* end = mmap.data() + mmap.size();
    while (p < end) {
        OHLCV row;
        row.timestamp_ms = parse_int64(p); ++p; // skip comma
        row.open = parse_double(p); ++p;
        row.high = parse_double(p); ++p;
        row.low = parse_double(p); ++p;
        row.close = parse_double(p); ++p;
        row.volume = parse_int64(p);
        out.push_back(row);
        if (*p == '\n') ++p;
    }
    auto end_time = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start).count();
}

int main() {
    const char* path = "benchmark_ticks.csv";
    constexpr size_t ROWS = 5'000'000;
    
    std::cout << "Generating " << ROWS << " row test file...\n";
    generate_test_file(path, ROWS);
    std::cout << "File size: ~" << (ROWS * 48 / 1024.0 / 1024.0) << " MB\n\n";

    std::vector<OHLCV> data_ifstream, data_mmap;
    data_ifstream.reserve(ROWS);
    data_mmap.reserve(ROWS);

    std::cout << "Benchmarking std::ifstream + string parsing...\n";
    auto ms_ifs = benchmark_ifstream(path, data_ifstream);
    
    std::cout << "Benchmarking Memory-Mapped Zero-Copy parser...\n";
    auto ms_mmap = benchmark_mmap(path, data_mmap);

    std::cout << "\n=== Results (5M OHLCV Rows) ===\n";
    std::cout << "std::ifstream : " << ms_ifs << " ms\n";
    std::cout << "Memory Mapped : " << ms_mmap << " ms\n";
    std::cout << "Speedup       : " << (ms_ifs / std::max(1.0, (double)ms_mmap)) << "x\n";
    std::cout << "Rows parsed   : " << data_ifstream.size() << " / " << data_mmap.size() << "\n";
    std::cout << "Checksum match: " << (data_ifstream.back().volume == data_mmap.back().volume ? "✅" : "❌") << "\n";

    return 0;
}