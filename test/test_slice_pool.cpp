#include "TwoBufQueue/src/base_components/block_slice/slice_pool.hpp"

#include <cassert>
#include <barrier>
#include <array>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace TwoBufQueue;
using ConcurrentSlice = AllocNReleaseNSlicePool::Slice;

static void byte_rollover() {
    AllocNReleaseNSlicePool pool(64, 2, 2);
    auto first = pool.allocate(40);
    assert(first);
    std::memset(first.data(), 42, first.size());
    auto second = pool.allocate(40); // Automatically seals the first block.
    assert(second && second.data() != first.data());
    assert(!pool.allocate(40)); // Both blocks are still held.
    for (std::size_t i = 0; i < first.size(); ++i)
        assert(first.data()[i] == std::byte{42});
    auto* recycled = first.data();
    std::thread consumer([slice = std::move(first)]() mutable { slice.release(); });
    consumer.join();
    auto retry = pool.allocate(64);
    assert(retry && retry.data() == recycled);
}

static void slot_rollover() {
    AllocNReleaseNSlicePool pool(64, 0, 1, {.max_slices_per_block = 2});
    for (int pass = 0; pass < 10; ++pass) {
        auto a = pool.allocate(1);
        auto b = pool.allocate(1);
        assert(a && b && b.data() == a.data() + 1);
        assert(!pool.allocate(1)); // Slots exhausted despite remaining bytes.
        std::thread t1([s = std::move(a)]() mutable { s.release(); });
        std::thread t2([s = std::move(b)]() mutable { s.release(); });
        t1.join(); t2.join();
    }
    assert(pool.allocate(64));
}

static void alignment_and_oversize() {
    for (std::size_t alignment : {1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
        AllocNReleaseNSlicePool pool(65, 1, 1);
        auto prefix = pool.allocate(1);
        assert(prefix);
        assert(!pool.allocate(66)); // Must not discard the usable current block.
        auto aligned = pool.allocate(1, alignment);
        assert(aligned);
        assert(reinterpret_cast<std::uintptr_t>(aligned.data()) % alignment == 0);
        assert(aligned.data() == prefix.data() + alignment);
    }
    AllocNReleaseNSlicePool pool(64, 2, 2);
    auto prefix = pool.allocate(63);
    auto aligned = pool.allocate(8, 8); // Padding alone consumes the old remainder.
    assert(prefix && aligned);
    assert(reinterpret_cast<std::uintptr_t>(aligned.data()) % 8 == 0);
}

static void concurrent_allocate() {
    constexpr std::size_t threads = 8, per_thread = 100;
    // Exercise both byte and slot exhaustion, including lazy block creation.
    for (std::size_t slots : {3u, 1024u}) {
        AllocNReleaseNSlicePool pool(64, 0, threads * per_thread,
                         {.max_slices_per_block = slots});
        for (int pass = 0; pass < 4; ++pass) {
            std::array<std::vector<ConcurrentSlice>, threads> held;
            std::barrier start(static_cast<std::ptrdiff_t>(threads));
            std::vector<std::thread> workers;
            for (std::size_t t = 0; t < threads; ++t) {
                workers.emplace_back([&, t] {
                    start.arrive_and_wait();
                    for (std::size_t i = 0; i < per_thread; ++i) {
                        auto s = pool.allocate(8, 8);
                        assert(s);
                        const std::uint64_t value = t * per_thread + i;
                        std::memcpy(s.data(), &value, sizeof(value));
                        held[t].push_back(std::move(s));
                        // release some slices while other threads allocate/switch blocks.
                        auto transient = pool.allocate(8, 8);
                        assert(transient);
                    }
                });
            }
            for (auto& worker : workers) worker.join();
            std::vector<std::uintptr_t> addresses;
            for (std::size_t t = 0; t < threads; ++t) {
                for (std::size_t i = 0; i < per_thread; ++i) {
                    std::uint64_t value;
                    std::memcpy(&value, held[t][i].data(), sizeof(value));
                    assert(value == t * per_thread + i);
                    addresses.push_back(reinterpret_cast<std::uintptr_t>(held[t][i].data()));
                }
            }
            std::sort(addresses.begin(), addresses.end());
            assert(std::adjacent_find(addresses.begin(), addresses.end()) == addresses.end());
            workers.clear();
            for (std::size_t t = 0; t < threads; ++t)
                workers.emplace_back([&, t] { held[t].clear(); });
            for (auto& worker : workers) worker.join();
        }
    }
}

static void no_lock_allocate() {
    Alloc1ReleaseNSlicePool pool(64, 1, 1);
    for (int i = 0; i < 100; ++i) {
        auto s = pool.allocate(64);
        assert(s);
        std::thread consumer([slice = std::move(s)]() mutable { slice.release(); });
        consumer.join();
    }
}

template<class PoolType>
static void common_contract() {
    // The exact same contract tests run against all three synchronization policies.
    for (std::size_t cap : {1u, 63u, 64u, 65u, 129u}) {
        PoolType pool(4096, 1, 1, {.max_slices_per_block = cap});
        for (int pass = 0; pass < 3; ++pass) {
            std::vector<typename PoolType::Slice> held;
            for (std::size_t i = 0; i < cap; ++i) {
                auto s = pool.allocate(1);
                assert(s);
                *s.data() = static_cast<std::byte>(i);
                held.push_back(std::move(s));
            }
            assert(!pool.allocate(1));
            for (std::size_t i = cap; i-- > 0;) {
                assert(*held[i].data() == static_cast<std::byte>(i));
                held[i].release();
            }
        }
    }
    for (std::size_t alignment : {1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
        PoolType pool(65, 1, 1);
        auto first = pool.allocate(1);
        assert(!pool.allocate(66));
        auto aligned = pool.allocate(1, alignment);
        assert(aligned && aligned.data() == first.data() + alignment);
        assert(reinterpret_cast<std::uintptr_t>(aligned.data()) % alignment == 0);
    }
    bool rejected = false;
    try { PoolType invalid(64, 0, 1, {.max_slices_per_block = 0}); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

static void simultaneous_modes() {
    Alloc1Release1SlicePool local(64, 1, 1);
    Alloc1ReleaseNSlicePool producer(64, 1, 1);
    AllocNReleaseNSlicePool shared(64, 1, 1);
    auto a = local.allocate(64);
    auto b = producer.allocate(64);
    auto c = shared.allocate(64);
    assert(a && b && c);
    assert(!local.allocate(1) && !producer.allocate(1) && !shared.allocate(1));
    std::thread consumer1([s = std::move(b)]() mutable { s.release(); });
    std::thread consumer2([s = std::move(c)]() mutable { s.release(); });
    a.release();
    consumer1.join(); consumer2.join();
    assert(local.allocate(64) && producer.allocate(64) && shared.allocate(64));
}

template<class PoolType>
static void close_and_notify() {
    std::atomic<unsigned> released{0};
    PoolType pool(64, 1, 1);
    pool.set_release_callback([](void* context) noexcept {
        static_cast<std::atomic<unsigned>*>(context)->fetch_add(1, std::memory_order_relaxed);
    }, &released);
    auto s = pool.allocate(1); // The last block is not full.
    assert(s && pool.block_count() == 1 && pool.in_use_count() == 1);
    pool.close();
    pool.close();
    assert(!pool.allocate(1) && released.load() == 0);
    s.release();
    assert(released.load() == 1 && pool.all_released()); // 最后一次释放立即归还并通知。
    pool.close();
    assert(released.load() == 1);
    pool.set_release_callback(nullptr, nullptr);
    PoolType empty(64, 0, 1);
    empty.close();
    assert(!empty.allocate(1) && empty.block_count() == 0);
}

int main() {
    byte_rollover();
    slot_rollover();
    alignment_and_oversize();
    concurrent_allocate();
    no_lock_allocate();
    common_contract<Alloc1Release1SlicePool>();
    common_contract<Alloc1ReleaseNSlicePool>();
    common_contract<AllocNReleaseNSlicePool>();
    simultaneous_modes();
    close_and_notify<Alloc1Release1SlicePool>();
    close_and_notify<Alloc1ReleaseNSlicePool>();
    close_and_notify<AllocNReleaseNSlicePool>();
    std::cout << "SlicePool rollover, backpressure, reuse, alignment and allocation lock policies passed\n";
}
