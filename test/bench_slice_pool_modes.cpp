#include "TwoBufQueue/src/base_components/block_slice/slice_pool.hpp"
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace TwoBufQueue;
using Clock = std::chrono::steady_clock;
constexpr std::size_t count = 100000, depth = 128, readers = 4;

template<class Pool, std::size_t Writers>
static void run(const char* name) {
    Pool pool(65536, 32, 32);
    std::array<std::vector<double>, Writers> samples;
    for (auto& sample : samples) sample.reserve(count / 256 + 1);
    auto report = [&](Clock::time_point begin) {
        const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        std::vector<double> all;
        for (const auto& sample : samples) all.insert(all.end(), sample.begin(), sample.end());
        std::sort(all.begin(), all.end());
        std::cout << name << ": " << count * Writers / seconds / 1e6
                  << " M slices/s, sampled allocate p99 " << all[all.size() * 99 / 100]
                  << " ns\n";
    };
    if constexpr (std::is_same_v<Pool, Alloc1Release1SlicePool>) {
        auto begin = Clock::now();
        for (std::size_t i = 0; i < count; ++i) {
            const bool sampled = (i & 255) == 0;
            const auto t = sampled ? Clock::now() : Clock::time_point{};
            auto s = pool.allocate(8, 8);
            if (!s) std::abort();
            if (sampled) samples[0].push_back(std::chrono::duration<double, std::nano>(Clock::now() - t).count());
            *s.data() = std::byte{42};
            std::atomic_signal_fence(std::memory_order_seq_cst); // Compiler barrier only.
        }
        report(begin);
    } else {
        struct Slot { typename Pool::Slice slice; std::atomic<bool> ready{false}; };
        auto slots = std::make_unique<Slot[]>(Writers * readers * depth);
        std::barrier start(static_cast<std::ptrdiff_t>(Writers + readers + 1));
        std::vector<std::thread> threads;
        for (std::size_t r = 0; r < readers; ++r) {
            threads.emplace_back([&, r] {
                std::array<std::size_t, Writers> consumed{};
                std::size_t total = 0;
                start.arrive_and_wait();
                while (total < Writers * count / readers) {
                    bool progress = false;
                    for (std::size_t w = 0; w < Writers; ++w) {
                        auto& slot = slots[(w * readers + r) * depth + consumed[w] % depth];
                        if (!slot.ready.load(std::memory_order_acquire)) continue;
                        if (*slot.slice.data() != std::byte{42}) std::abort();
                        slot.slice.release();
                        slot.ready.store(false, std::memory_order_release);
                        ++consumed[w]; ++total; progress = true;
                    }
                    if (!progress) std::this_thread::yield();
                }
            });
        }
        for (std::size_t w = 0; w < Writers; ++w) {
            threads.emplace_back([&, w] {
                start.arrive_and_wait();
                for (std::size_t i = 0; i < count; ++i) {
                    auto& slot = slots[(w * readers + i % readers) * depth + (i / readers) % depth];
                    while (slot.ready.load(std::memory_order_acquire)) std::this_thread::yield();
                    const bool sampled = (i & 255) == 0;
                    const auto t = sampled ? Clock::now() : Clock::time_point{};
                    auto s = pool.allocate(8, 8);
                    while (!s) { std::this_thread::yield(); s = pool.allocate(8, 8); }
                    if (sampled) samples[w].push_back(std::chrono::duration<double, std::nano>(Clock::now() - t).count());
                    *s.data() = std::byte{42};
                    slot.slice = std::move(s);
                    slot.ready.store(true, std::memory_order_release);
                }
            });
        }
        auto begin = Clock::now();
        start.arrive_and_wait();
        for (auto& thread : threads) thread.join();
        report(begin);
    }
}

int main() {
    for (int pass = 0; pass < 3; ++pass) {
        run<Alloc1Release1SlicePool, 1>("Alloc1Release1 (same thread)");
        run<Alloc1ReleaseNSlicePool, 1>("Alloc1ReleaseN (1 writer, 4 readers)");
        run<AllocNReleaseNSlicePool, 4>("AllocNReleaseN (4 writers, 4 readers)");
    }
}
