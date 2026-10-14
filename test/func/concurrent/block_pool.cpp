#include "../../../support/joining_thread.hpp"
#include "../../support/warm_pool_test_support.hpp"
#include "../../support/block_test_support.hpp"
#include "common.hpp"
#include <array>
#include <barrier>

namespace ct {
namespace {
using Pool = TestBlockPool<ReaderBlock<>>;
static_assert(std::is_same_v<Pool, TestBlockPool<ReaderBlock<>, true>>);

template<class Predicate>
void until(Predicate predicate) {
    const auto deadline = Clock::now() + 5s;
    while (!predicate()) {
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
}

struct Work {
    ReaderBlock<>* block;
    ReaderBlock<>::Slice slice;
    Bytes expected;
    unsigned writer, sequence;
};

template<bool NAlloc, bool NRelease, bool External = false>
void streaming(std::size_t initial) {
    constexpr unsigned writers = NAlloc ? 4 : 1;
    constexpr unsigned readers = NRelease ? 4 : 1;
    constexpr unsigned count = 1001;
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>, NAlloc>>(256, initial, 8,
                                             {.max_slices_per_block = 1});
    Channel<Work> handoff(8);
    Failure failure;
    std::mutex live_mutex, external_mutex;
    std::set<ReaderBlock<>*> live;
    std::array<std::array<unsigned, count>, writers> seen{};
    std::atomic<unsigned> completed{0}, active{writers}, overlapped{0};
    std::vector<brq_support::JoiningThread> consumers;
    for (unsigned r = 0; r < readers; ++r) {
        consumers.emplace_back([&, r] {
            while (auto work = handoff.pop()) {
                failure.run([&] {
                    equal_bytes(work->slice.data(), work->slice.size(), work->expected, "direct pool");
                    for (unsigned i = 0; i <= r; ++i) std::this_thread::yield();
                    equal_bytes(work->slice.data(), work->slice.size(), work->expected, "direct pool held");
                    std::memset(work->slice.data(), 0xa5, work->slice.size());
                    {
                        std::lock_guard lock(live_mutex);
                        CHECK(live.erase(work->block) == 1);
                        CHECK(++seen[work->writer][work->sequence] == 1);
                    }
                });
                // This is the only return path, including for the N/1 configuration.
                work->slice.release();
                if (active.load() != 0) overlapped.fetch_add(1);
                completed.fetch_add(1);
            }
        });
    }
    std::barrier start(static_cast<std::ptrdiff_t>(writers));
    std::vector<brq_support::JoiningThread> producers;
    for (unsigned w = 0; w < writers; ++w) {
        producers.emplace_back([&, w] {
            start.arrive_and_wait();
            failure.run([&] {
                const auto deadline = Clock::now() + 30s;
                const auto get = [&]() {
                    if constexpr (External) {
                        std::lock_guard lock(external_mutex);
                        return pool.get().get();
                    } else return pool.get().get();
                };
                for (unsigned seq = 0; seq < count && !failure.failed.load(); ++seq) {
                    auto* block = get();
                    while (!block) {
                        if (failure.failed.load()) return;
                        CHECK(Clock::now() < deadline);
                        std::this_thread::yield();
                        block = get();
                    }
                    {
                        std::lock_guard lock(live_mutex);
                        CHECK(live.insert(block).second);
                    }
                    CHECK(pool.resource_count() <= 8 && pool.in_use_count() <= 8);
                    const auto size = std::array<std::size_t, 5>{16, 17, 31, 63, 127}[seq % 5];
                    auto slice = block->allocate(size, 8);
                    CHECK(slice);
                    auto expected = message(w, seq, size);
                    std::memcpy(slice.data(), expected.data(), size);
                    // Keep a live slice, so completing on this thread cannot return the block.
                    finish_block(block);
                    Work work{block, std::move(slice), std::move(expected), w, seq};
                    while (!handoff.push(work)) {
                        CHECK(Clock::now() < deadline);
                        std::this_thread::yield();
                    }
                    if (seq == 16) {
                        while (completed.load() == 0) {
                            CHECK(Clock::now() < deadline);
                            std::this_thread::yield();
                        }
                    }
                }
            });
            active.fetch_sub(1);
        });
    }
    for (auto& producer : producers) producer.join();
    handoff.close();
    for (auto& consumer : consumers) consumer.join();
    failure.check();
    CHECK(completed.load() == writers * count && overlapped.load() > 0);
    CHECK(live.empty() && pool.in_use_count() == 0 && pool.resource_count() <= 8);
    for (const auto& row : seen) for (auto visits : row) CHECK(visits == 1);
    std::cout << "direct blocks verified=" << completed.load() << " blocks=" << pool.resource_count() << '\n';
}

void same_thread() {
    auto pool = make_block_pool<TestBlockPool<Alloc1Release1ReaderBlock, false>>(64, 0, 3,
                                                          {.max_slices_per_block = 1});
    CHECK(pool.preallocate(3));
    CHECK(!pool.preallocate(4));
    for (unsigned round = 0; round < 128; ++round) {
        std::array<Alloc1Release1ReaderBlock*, 3> blocks{};
        for (auto& block : blocks) { block = pool.get().get(); CHECK(block); }
        CHECK(pool.in_use_count() == 3 && pool.get().get() == nullptr);
        CHECK(std::set(blocks.begin(), blocks.end()).size() == 3);
        for (auto* block : blocks) {
            auto slice = block->allocate(64);
            const auto expected = message(0, round, 64);
            std::memcpy(slice.data(), expected.data(), expected.size());
            equal_bytes(slice.data(), slice.size(), expected, "same thread");
            if ((round & 1) == 0) { finish_block(block); slice.release(); }
            else { slice.release(); finish_block(block); }
        }
        CHECK(pool.in_use_count() == 0 && pool.resource_count() == 3);
    }
}

void held_block() {
    auto pool = make_block_pool<Pool>(128, 2, 2, {.max_slices_per_block = 1});
    auto* pinned = pool.get().get();
    auto held = pinned->allocate(128);
    const auto expected = message(9, 123, 128);
    std::memcpy(held.data(), expected.data(), expected.size());
    finish_block(pinned);
    Failure failure;
    std::atomic<unsigned> completed{0}, active{4};
    std::barrier start(4);
    std::vector<brq_support::JoiningThread> workers;
    for (unsigned w = 0; w < 4; ++w) workers.emplace_back([&, w] {
        start.arrive_and_wait();
        failure.run([&] {
            const auto deadline = Clock::now() + 30s;
            for (unsigned seq = 0; seq < 257; ++seq) {
                auto* block = pool.get().get();
                while (!block) {
                    CHECK(Clock::now() < deadline);
                    std::this_thread::yield();
                    block = pool.get().get();
                }
                CHECK(block != pinned);
                auto slice = block->allocate(128);
                const auto value = message(w, seq, 128);
                std::memcpy(slice.data(), value.data(), value.size());
                std::this_thread::yield();
                equal_bytes(slice.data(), slice.size(), value, "competing for one free block");
                slice.release();
                finish_block(block); // Return from several allocation threads, under the return lock.
                completed.fetch_add(1);
            }
        });
        active.fetch_sub(1);
    });
    failure.run([&] {
        const auto deadline = Clock::now() + 30s;
        do {
            equal_bytes(held.data(), held.size(), expected, "pinned block");
            CHECK(pool.resource_count() == 2 && Clock::now() < deadline);
            std::this_thread::yield();
        } while (active.load() != 0);
    });
    for (auto& worker : workers) worker.join();
    held.release();
    failure.check();
    CHECK(completed.load() == 4 * 257 && pool.in_use_count() == 0);
}

void wait_ready_blocks() {
    constexpr unsigned count = 8;
    auto pool = make_block_pool<Pool>(64, count, count);
    std::array<ReaderBlock<>*, count> held{}, acquired{};
    for (auto& block : held) { block = pool.get().get(); CHECK(block); }
    for (auto* block : held) finish_block(block);
    CHECK(pool.in_use_count() == 0 && pool.all_released());
    Failure failure;
    std::barrier start(count);
    std::vector<brq_support::JoiningThread> workers;
    for (unsigned i = 0; i < count; ++i) workers.emplace_back([&, i] {
        start.arrive_and_wait();
        failure.run([&] { acquired[i] = pool.wait_get(10s).get(); CHECK(acquired[i]); });
    });
    for (auto& worker : workers) worker.join();
    std::set<ReaderBlock<>*> unique;
    for (auto* block : acquired) if (block) { unique.insert(block); finish_block(block); }
    failure.check();
    CHECK(unique.size() == count && pool.all_released());
}

void wait_burst() {
    constexpr unsigned count = 8;
    auto pool = make_block_pool<Pool>(64, count, count);
    std::array<ReaderBlock<>*, count> held{}, acquired{};
    for (auto& block : held) { block = pool.get().get(); CHECK(block); }
    std::atomic<unsigned> entered{0};
    std::atomic<unsigned> done{0};
    Failure failure;
    std::vector<brq_support::JoiningThread> workers;
    for (unsigned i = 0; i < count; ++i) workers.emplace_back([&, i] {
        failure.run([&] {
            entered.fetch_add(1);
            acquired[i] = pool.wait_get(10s).get();
            CHECK(acquired[i]);
        });
        done.fetch_add(1);
    });
    until([&] { return entered.load() == count; });
    std::this_thread::sleep_for(10ms);
    CHECK(done.load() == 0);
    for (auto* block : held) finish_block(block);
    until([&] { return done.load() == count; });
    for (auto& worker : workers) worker.join();
    std::set<ReaderBlock<>*> unique;
    for (auto* block : acquired) if (block) { unique.insert(block); finish_block(block); }
    failure.check();
    CHECK(unique.size() == count && pool.in_use_count() == 0);
}

void wait_timeout() {
    auto pool = make_block_pool<Pool>(64, 1, 1);
    auto* held = pool.get().get();
    CHECK(held);
    std::atomic<bool> done{false};
    brq_support::JoiningThread notifier([&] {
        while (!done.load()) {

            std::this_thread::yield();
        }
    });
    const auto begin = Clock::now();
    auto* result = pool.wait_get(50ms).get();
    const auto elapsed = Clock::now() - begin;
    done.store(true);
    notifier.join();
    finish_block(held);
    CHECK(result == nullptr);
    CHECK(elapsed >= 50ms && elapsed < 2s);
    CHECK(pool.resource_count() == 1 && pool.in_use_count() == 0);
}

void wait_nonpositive() {
    auto pool = make_block_pool<Pool>(64, 1, 1);
    for (const auto timeout : {0ms, -1ms, std::chrono::milliseconds::min()}) {
        auto* block = pool.wait_get(timeout).get();
        CHECK(block);
        auto slice = block->allocate(64);
        CHECK(slice);
        std::memset(slice.data(), 0xa5, slice.size());
        const auto begin = Clock::now();
        auto* unavailable = pool.wait_get(timeout).get();
        CHECK(Clock::now() - begin < 1s);
        CHECK(unavailable == nullptr);
        for (std::size_t i = 0; i < slice.size(); ++i) CHECK(slice.data()[i] == std::byte{0xa5});
        finish_block(block);
        slice.release();
        CHECK(pool.in_use_count() == 0);
    }
}

void wait_independent() {
    auto pool = make_block_pool<Pool>(64, 1, 1);
    auto* held = pool.get().get();
    CHECK(held);
    ReaderBlock<>* long_result = nullptr;
    ReaderBlock<>* short_result = nullptr;
    std::atomic<bool> long_done{false}, entered{false};
    brq_support::JoiningThread long_waiter([&] {
        entered.store(true);
        long_result = pool.wait_get(5s).get();
        long_done.store(true);
    });
    until([&] { return entered.load(); });
    Clock::duration short_elapsed{};
    brq_support::JoiningThread short_waiter([&] {
        const auto begin = Clock::now();
        short_result = pool.wait_get(50ms).get();
        short_elapsed = Clock::now() - begin;
    });
    short_waiter.join();
    const bool still_waiting = !long_done.load();
    finish_block(held);
    long_waiter.join();
    if (long_result) finish_block(long_result);
    CHECK(short_result == nullptr && long_result == held && still_waiting);
    CHECK(short_elapsed >= 50ms && short_elapsed < 2s);
    CHECK(pool.in_use_count() == 0);
}

template<class Duration>
void wait_max(Duration timeout) {
    auto pool = make_block_pool<Pool>(64, 1, 1);
    auto* held = pool.get().get();
    CHECK(held);
    ReaderBlock<>* result = nullptr;
    std::atomic<bool> done{false}, entered{false};
    brq_support::JoiningThread waiter([&] {
        entered.store(true);
        result = pool.wait_get(timeout).get();
        done.store(true);
    });
    until([&] { return entered.load(); });
    std::this_thread::sleep_for(10ms);
    const bool was_waiting = !done.load();
    finish_block(held);
    waiter.join();
    if (result) finish_block(result);
    CHECK(was_waiting && result == held && pool.in_use_count() == 0);
}
}

void block_pool_cases(Cases& cases) {
    cases.push_back({"direct_pool_same_thread", same_thread});
    cases.push_back({"direct_pool_cross_11", [] { streaming<false, false>(8); }});
    cases.push_back({"direct_pool_1N", [] { streaming<false, true>(8); }});
    cases.push_back({"direct_pool_N1", [] { streaming<true, false>(8); }});
    cases.push_back({"direct_pool_NN_preallocated", [] { streaming<true, true>(8); }});
    cases.push_back({"direct_pool_NN_growth", [] { streaming<true, true>(0); }});
    cases.push_back({"direct_pool_external_lock", [] { streaming<true, true, true>(8); }});
    cases.push_back({"direct_pool_held_block", held_block});
    cases.push_back({"direct_pool_wait_ready_blocks", wait_ready_blocks});
    cases.push_back({"direct_pool_wait_burst", wait_burst});
    cases.push_back({"direct_pool_wait_timeout", wait_timeout});
    cases.push_back({"direct_pool_wait_nonpositive", wait_nonpositive});
    cases.push_back({"direct_pool_wait_independent", wait_independent});
    cases.push_back({"direct_pool_wait_max_millis", [] { wait_max(std::chrono::milliseconds::max()); }});
    cases.push_back({"direct_pool_wait_max_ticks", [] { wait_max(Clock::duration::max()); }});
}
} // namespace ct
