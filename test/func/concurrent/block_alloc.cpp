#include "../../../support/joining_thread.hpp"
#include "../../warm_pool_test_support.hpp"
#include "../../block_test_support.hpp"
#include "common.hpp"

namespace ct {
using SharedBlock = AllocNReleaseNReaderBlock;
using SharedPool = TestBlockPool<SharedBlock>;

// No SlicePool here: workers exercise the block's own allocation lock directly.
static void direct_exhaustion(std::size_t slots, bool byte_limit) {
    constexpr unsigned threads = 8;
    const std::size_t size = byte_limit ? 16 : 1;
    const std::size_t limit = byte_limit ? 512 : slots;
    const std::size_t bytes = byte_limit ? 8192 : 4096;
    auto pool = make_block_pool<SharedPool>(bytes, 1, 1, {.max_slices_per_block = slots});
    std::atomic<unsigned> recycled{0};
    pool.set_release_callback([](void *context) noexcept {
        static_cast<std::atomic<unsigned> *>(context)->fetch_add(1);
    }, &recycled);
    for (unsigned round = 0; round < 16; ++round) {
        auto *block = pool.get().get();
        CHECK(block);
        using Work = std::pair<SharedBlock::Slice, Bytes>;
        std::array<std::vector<Work>, threads> held;
        Failure failure;
        std::barrier gate(static_cast<std::ptrdiff_t>(threads));
        std::vector<brq_support::JoiningThread> workers;
        for (unsigned w = 0; w < threads; ++w)
            workers.emplace_back([&, w] {
                gate.arrive_and_wait();
                failure.run([&] {
                    bool exhausted = false;
                    for (std::size_t i = 0; i <= limit; ++i) {
                        auto slice = block->allocate(size, size);
                        if (!slice) {
                            exhausted = true;
                            break;
                        }
                        CHECK(reinterpret_cast<std::uintptr_t>(slice.data()) % size == 0);
                        Bytes expected(size, std::byte((w * 37 + i + round) & 255));
                        std::memcpy(slice.data(), expected.data(), size);
                        held[w].emplace_back(std::move(slice), std::move(expected));
                    }
                    CHECK(exhausted);
                    CHECK(!block->allocate(size, size));
                });
            });
        for (auto &worker : workers)
            worker.join();
        failure.check();
        std::vector<std::uintptr_t> addresses;
        for (const auto &row : held)
            for (const auto &work : row) {
                equal_bytes(work.first.data(), work.first.size(), work.second, "direct block");
                addresses.push_back(reinterpret_cast<std::uintptr_t>(work.first.data()));
            }
        CHECK(addresses.size() == limit);
        std::sort(addresses.begin(), addresses.end());
        for (std::size_t i = 1; i < addresses.size(); ++i)
            CHECK(addresses[i] == addresses[i - 1] + size);
        CHECK(block->outstanding_count() == limit);
        if ((round & 1) == 0)
            finish_block(block); // Allocation calls have all returned; releases may now race.
        CHECK(recycled.load() == round);
        workers.clear();
        std::barrier release_gate(static_cast<std::ptrdiff_t>(threads));
        for (unsigned w = 0; w < threads; ++w)
            workers.emplace_back([&, w] {
                release_gate.arrive_and_wait();
                for (auto it = held[w].rbegin(); it != held[w].rend(); ++it)
                    it->first.release();
            });
        for (auto &worker : workers)
            worker.join();
        if ((round & 1) != 0) {
            CHECK(block->outstanding_count() == 0);
            CHECK(pool.in_use_count() == 1);
            CHECK(recycled.load() == round);
            finish_block(block);
        }
        CHECK(pool.in_use_count() == 0);
        CHECK(recycled.load() == round + 1);
    }
    pool.set_release_callback(nullptr, nullptr);
}

static void direct_streaming() {
    constexpr unsigned producers = 4, count = 257;
    constexpr std::size_t total = producers * count;
    // Each request needs at most 127 payload + 63 padding bytes.
    auto pool = make_block_pool<SharedPool>(total * 192, 1, 1, {.max_slices_per_block = total});
    auto *block = pool.get().get();
    CHECK(block);
    struct Work {
        SharedBlock::Slice slice;
        Bytes expected;
        unsigned writer, seq;
    };
    Channel<Work> channel(128);
    Failure failure;
    std::array<std::array<unsigned, count>, producers> seen{};
    std::mutex seen_mutex;
    std::array<std::vector<std::pair<std::uintptr_t, std::size_t>>, producers> ranges;
    std::atomic<unsigned> completed{0}, active{producers}, overlapped{0};
    std::vector<brq_support::JoiningThread> consumers;
    for (unsigned i = 0; i < 4; ++i)
        consumers.emplace_back([&] {
            while (auto work = channel.pop()) {
                failure.run([&] {
                    equal_bytes(work->slice.data(), work->slice.size(), work->expected, "direct streaming");
                    std::this_thread::yield();
                    equal_bytes(work->slice.data(), work->slice.size(), work->expected, "direct delayed");
                    std::lock_guard lock(seen_mutex);
                    CHECK(++seen[work->writer][work->seq] == 1);
                });
                work->slice.release();
                if (active.load() > 0)
                    overlapped.fetch_add(1);
                completed.fetch_add(1);
            }
        });
    std::barrier gate(static_cast<std::ptrdiff_t>(producers));
    std::vector<brq_support::JoiningThread> allocators;
    for (unsigned w = 0; w < producers; ++w)
        allocators.emplace_back([&, w] {
            gate.arrive_and_wait();
            failure.run([&] {
                const auto deadline = Clock::now() + 30s;
                for (unsigned seq = 0; seq < count && !failure.failed.load(); ++seq) {
                    const auto size = std::array<std::size_t, 5>{16, 17, 31, 63, 127}[seq % 5];
                    const auto alignment = std::size_t{1} << ((seq + w) % 7);
                    auto slice = block->allocate(size, alignment);
                    CHECK(slice);
                    CHECK(reinterpret_cast<std::uintptr_t>(slice.data()) % alignment == 0);
                    ranges[w].emplace_back(reinterpret_cast<std::uintptr_t>(slice.data()), size);
                    Work work{std::move(slice), message(w, seq, size), w, seq};
                    std::memcpy(work.slice.data(), work.expected.data(), size);
                    while (!channel.push(work)) {
                        if (failure.failed.load())
                            return;
                        if (Clock::now() > deadline)
                            throw std::runtime_error("direct block handoff deadline");
                        std::this_thread::yield();
                    }
                    if (seq == 32)
                        while (completed.load() == 0 && !failure.failed.load()) {
                            if (Clock::now() > deadline)
                                throw std::runtime_error("direct block release overlap deadline");
                            std::this_thread::yield();
                        }
                }
            });
            active.fetch_sub(1);
        });
    for (auto &worker : allocators)
        worker.join();
    const bool exhausted = !block->allocate(1); // Slot limit, with bytes still available.
    finish_block(block);
    channel.close();
    for (auto &worker : consumers)
        worker.join();
    failure.check();
    CHECK(exhausted);
    CHECK(completed.load() == total);
    CHECK(overlapped.load() > 0);
    CHECK(pool.in_use_count() == 0);
    std::vector<std::pair<std::uintptr_t, std::size_t>> all;
    for (const auto &row : ranges)
        all.insert(all.end(), row.begin(), row.end());
    CHECK(all.size() == total);
    std::sort(all.begin(), all.end());
    for (std::size_t i = 1; i < all.size(); ++i)
        CHECK(all[i - 1].first + all[i - 1].second <= all[i].first);
    for (const auto &row : seen)
        for (auto n : row)
            CHECK(n == 1);
}

template<class Block> static void without_lock_contract() {
    auto pool = make_block_pool<TestBlockPool<Block, false>>(128, 1, 1, {.max_slices_per_block = 4});
    for (unsigned round = 0; round < 3; ++round) {
        auto *block = pool.get().get();
        CHECK(block);
        auto first = block->allocate(1);
        CHECK(first);
        CHECK(!block->allow_without_lock(129));
        auto second = block->allow_without_lock(3, 8);
        CHECK(second && second.data() == first.data() + 8);
        auto third = block->allow_without_lock(5);
        if (third) std::memset(third.data(), 0x5a, third.size());
        CHECK(third && third.data() == first.data() + 11);
        for (std::size_t i = 0; i < third.size(); ++i) CHECK(third.data()[i] == std::byte{0x5a});
        auto fourth = block->allocate(1);
        CHECK(fourth && fourth.data() == first.data() + 16);
        second.release();
        third.release();
        CHECK(!block->allow_without_lock(1)); // Release must not reuse an index.
        CHECK(block->outstanding_count() == 2);
        finish_block(block);
        if constexpr (Block::concurrent) {
            brq_support::JoiningThread a([slice = std::move(first)]() mutable { slice.release(); });
            brq_support::JoiningThread b([slice = std::move(fourth)]() mutable { slice.release(); });
        } else {
            first.release();
            fourth.release();
        }
        CHECK(pool.in_use_count() == 0);
    }
}

void block_alloc_cases(Cases &cases) {
    for (std::size_t slots : {1u, 63u, 64u, 65u, 129u})
        cases.push_back({"block_NN_slots_" + std::to_string(slots), [=] {
                             direct_exhaustion(slots, false);
                         }});
    cases.push_back({"block_NN_bytes", [] { direct_exhaustion(8193, true); }});
    cases.push_back({"block_NN_streaming", direct_streaming});
    cases.push_back({"block_without_lock_11", without_lock_contract<Alloc1Release1ReaderBlock>});
    cases.push_back({"block_without_lock_1N", without_lock_contract<Alloc1ReleaseNReaderBlock>});
    cases.push_back({"block_without_lock_NN", without_lock_contract<AllocNReleaseNReaderBlock>});
}
} // namespace ct
