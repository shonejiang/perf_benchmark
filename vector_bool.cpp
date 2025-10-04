#include <iostream>
#include <vector>
#include <chrono>
#include <numeric>
#include <random>
#include <algorithm>
#include <memory>
#include <cstring> // For std::memset

// --- Helper Timer Class ---
class Timer {
public:
    Timer(const std::string& name) : name_(name), start_time_(std::chrono::high_resolution_clock::now()) {}

    ~Timer() {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time_).count();
        double ms = duration / 1e6;
        std::cout << "[" << name_ << "] took " << ms << " ms." << std::endl;
    }

private:
    std::string name_;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time_;
};

// --- Custom Allocator with Memory Pre-faulting ---
template <typename T>
struct PreFaultAllocator {
    using value_type = T;

    PreFaultAllocator() = default;

    template <typename U>
    constexpr PreFaultAllocator(const PreFaultAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_alloc();
        }
        
        const size_t bytes_to_allocate = n * sizeof(T);
        if (auto p = static_cast<T*>(std::malloc(bytes_to_allocate))) {
            // Pre-fault the memory pages by writing to them.
            // This forces the OS to map virtual pages to physical RAM immediately.
            std::memset(p, 0, bytes_to_allocate);
            // std::cout << "Allocator: Allocated and pre-faulted " << bytes_to_allocate << " bytes." << std::endl;
            return p;
        }

        throw std::bad_alloc();
    }

    void deallocate(T* p, std::size_t /*n*/) noexcept {
        // std::cout << "Allocator: Deallocating memory." << std::endl;
        std::free(p);
    }
};

template <typename T, typename U>
bool operator==(const PreFaultAllocator<T>&, const PreFaultAllocator<U>&) { return true; }

template <typename T, typename U>
bool operator!=(const PreFaultAllocator<T>&, const PreFaultAllocator<U>&) { return false; }


// --- Main Test Logic ---
int main() {
    constexpr size_t ITEM_COUNT = 8096;
    constexpr size_t ACCESS_COUNT = 20'000'000; // 20 million random accesses

    // 1. Generate a shared list of random indices for a fair comparison
    std::cout << "Generating " << ACCESS_COUNT << " random indices..." << std::endl;
    std::vector<size_t> indices(ACCESS_COUNT);
    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<size_t> distrib(0, ITEM_COUNT - 1);
    for (size_t i = 0; i < ACCESS_COUNT; ++i) {
        indices[i] = distrib(gen);
    }
    std::cout << "Done generating indices.\n" << std::endl;

    // Volatile variable to prevent compiler from optimizing away the reads
    volatile bool sink = false;

    // --- Case 2: Raw contiguous memory ---
    {
        PreFaultAllocator<uint8_t> allocator;
        // Allocate 8096 bytes. We use uint8_t to represent a byte.
        uint8_t* raw_mem = allocator.allocate(ITEM_COUNT);

        {
            Timer timer("Case 2: Raw Memory (uint8_t*)");
            for (size_t i = 0; i < ACCESS_COUNT; ++i) {
                size_t idx = indices[i];
                raw_mem[idx] = 1 - raw_mem[idx]; // Read and write (simulating bool)
                sink = (raw_mem[idx] != 0);      // Another read
            }
        }
        
        allocator.deallocate(raw_mem, ITEM_COUNT);
    }

    // --- Case 1: std::vector<bool> ---
    {
        // Note: std::vector<bool> uses the allocator for its internal packed block type, not for `bool` itself.
        // The pre-faulting logic will still apply to the underlying memory blocks.
        std::vector<bool, PreFaultAllocator<bool>> bool_vec(ITEM_COUNT, PreFaultAllocator<bool>());
        
        Timer timer("Case 1: std::vector<bool>");
        for (size_t i = 0; i < ACCESS_COUNT; ++i) {
            size_t idx = indices[i];
            bool_vec[idx] = !bool_vec[idx]; // Read and write
            sink = bool_vec[idx];           // Another read
        }
    }

    std::cout << std::endl;


    return 0;
}


// g++ vector_bool.cpp -std=c++23 -O3 -march=native && taskset -c 31 ./a.out 
// Generating 20000000 random indices...
// Done generating indices.

// [Case 2: Raw Memory (uint8_t*)] took 15.9348 ms.
// [Case 1: std::vector<bool>] took 97.7998 ms.