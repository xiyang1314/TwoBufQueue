#include "../../../support/joining_thread.hpp"
#include "common.hpp"
namespace ct {
template <class Slice> struct SliceWork {
    Slice slice;
    Bytes expected;
    unsigned writer, seq;
};
struct LiveRanges {
    std::mutex mutex;
    std::map<std::uintptr_t, std::uintptr_t> ranges;
    void add(const std::byte *data, std::size_t size) {
        std::lock_guard lock(mutex);
        const auto begin = reinterpret_cast<std::uintptr_t>(data), end = begin + size;
        auto next = ranges.lower_bound(begin);
        CHECK(next == ranges.end() || end <= next->first);
        CHECK(next == ranges.begin() || std::prev(next)->second <= begin);
        CHECK(ranges.emplace(begin, end).second);
    }
    void remove(const std::byte *data) {
        std::lock_guard lock(mutex);
        CHECK(ranges.erase(reinterpret_cast<std::uintptr_t>(data)) == 1);
    }
};

template <class Pool> void streaming(unsigned producers, std::size_t slots) {
    Pool pool(8192, 8, 8, {.max_slices_per_block = slots});
    using Work = SliceWork<typename Pool::Slice>;
    Channel<Work> channel(128);
    Failure failure;
    LiveRanges live;
    constexpr unsigned count = 2001;
    std::vector<std::vector<unsigned>> seen(producers, std::vector<unsigned>(count));
    std::mutex seen_mutex;
    std::atomic<unsigned> active{producers}, overlapped{0}, completed{0}, retries{0};
    std::vector<brq_support::JoiningThread> consumers;
    for (unsigned worker = 0; worker < 4; ++worker)
        consumers.emplace_back([&, worker] {
            while (auto work = channel.pop()) {
                failure.run([&] {
                    const auto context = "slice writer=" + std::to_string(work->writer) +
                                         " seq=" + std::to_string(work->seq);
                    equal_bytes(work->slice.data(), work->slice.size(), work->expected, context);
                    for (unsigned i = 0; i < worker + 1; ++i)
                        std::this_thread::yield();
                    equal_bytes(work->slice.data(), work->slice.size(), work->expected,
                                context + " delayed");
                    {
                        std::lock_guard lock(seen_mutex);
                        CHECK(++seen[work->writer][work->seq] == 1);
                    }
                    std::memset(work->slice.data(), 0xa5, work->slice.size());
                    live.remove(work->slice.data());
                });
                work->slice.release();
                if (active.load() > 0)
                    overlapped.fetch_add(1);
                completed.fetch_add(1);
            }
        });
    std::barrier gate(static_cast<std::ptrdiff_t>(producers + 1));
    std::vector<brq_support::JoiningThread> allocators;
    for (unsigned w = 0; w < producers; ++w)
        allocators.emplace_back([&, w] {
            gate.arrive_and_wait();
            failure.run([&] {
                const auto deadline = Clock::now() + 30s;
                for (unsigned seq = 0; seq < count && !failure.failed.load(); ++seq) {
                    const auto size = std::array<std::size_t, 5>{16, 17, 31, 63, 127}[seq % 5];
                    const auto alignment = std::size_t{1} << ((seq + w) % 7);
                    auto slice = pool.allocate(size, alignment);
                    while (!slice) {
                        if (failure.failed.load())
                            return;
                        if (Clock::now() > deadline)
                            throw std::runtime_error("slice allocation deadline");
                        retries.fetch_add(1);
                        std::this_thread::yield();
                        slice = pool.allocate(size, alignment);
                    }
                    CHECK(reinterpret_cast<std::uintptr_t>(slice.data()) % alignment == 0);
                    live.add(slice.data(), slice.size());
                    Work work{std::move(slice), message(w, seq, size), w, seq};
                    std::memcpy(work.slice.data(), work.expected.data(), size);
                    while (!channel.push(work)) {
                        if (Clock::now() > deadline)
                            throw std::runtime_error("slice handoff deadline");
                        std::this_thread::yield();
                    }
                    // Force real allocate/release overlap instead of relying on scheduler luck.
                    if (seq == 32)
                        while (completed.load() == 0 && !failure.failed.load()) {
                            if (Clock::now() > deadline)
                                throw std::runtime_error("release overlap deadline");
                            std::this_thread::yield();
                        }
                }
            });
            active.fetch_sub(1);
        });
    gate.arrive_and_wait();
    for (auto &t : allocators)
        t.join();
    pool.close();
    channel.close();
    for (auto &t : consumers)
        t.join();
    failure.check();
    CHECK(overlapped.load() > 0);
    CHECK(completed.load() == producers * count);
    CHECK(live.ranges.empty());
    CHECK(pool.all_released());
    CHECK(pool.block_count() == 8);
    for (auto &row : seen)
        for (auto n : row)
            CHECK(n == 1);
    std::cout << "verified slices=" << completed << " releases during allocation=" << overlapped
              << " allocation retries=" << retries << '\n';
}

template <class Pool> void pinned() {
    Pool pool(128, 2, 2, {.max_slices_per_block = 1});
    Failure failure;
    for (unsigned round = 0; round < 100; ++round) {
        auto a = pool.allocate(128), b = pool.allocate(128);
        CHECK(a && b);
        const auto old = message(0, round, 128), keep = message(1, round, 128),
                   fresh = message(2, round, 128);
        std::memcpy(a.data(), old.data(), 128);
        std::memcpy(b.data(), keep.data(), 128);
        auto *held = b.data();
        auto *freed = a.data();
        std::atomic<bool> exhausted{false}, released{false}, rewritten{false}, checked{false};
        const auto deadline = Clock::now() + 10s;
        brq_support::JoiningThread allocator([&] {
            failure.run([&] {
                for (unsigned i = 0; i < 128; ++i)
                    CHECK(!pool.allocate(128));
                exhausted = true;
                wait_flag(released, failure, deadline);
                if (failure.failed.load())
                    return;
                auto replacement = pool.allocate(128);
                CHECK(replacement);
                CHECK(replacement.data() == freed);
                CHECK(replacement.data() != held);
                std::memcpy(replacement.data(), fresh.data(), 128);
                rewritten = true;
                wait_flag(checked, failure, deadline);
                equal_bytes(replacement.data(), 128, fresh, "replacement");
            });
        });
        failure.run([&] {
            wait_flag(exhausted, failure, deadline);
            equal_bytes(a.data(), 128, old, "pinned first");
            equal_bytes(b.data(), 128, keep, "pinned second");
            a.release();
            released = true;
            wait_flag(rewritten, failure, deadline);
            equal_bytes(b.data(), 128, keep, "pinned second after other block overwrite");
            checked = true;
        });
        allocator.join();
        b.release();
        failure.check();
    }
    pool.close();
    CHECK(pool.all_released());
}
void pool_cases(Cases &cases) {
    for (std::string mode : {"1N", "NN"}) {
        for (std::size_t slots : {1u, 64u, 65u})
            cases.push_back({"pool_" + mode + "_" + std::to_string(slots), [=] {
                                 if (mode == "1N")
                                     streaming<Alloc1ReleaseNSlicePool>(1, slots);
                                 else
                                     streaming<AllocNReleaseNSlicePool>(4, slots);
                             }});
        cases.push_back({"pin_" + mode, [=] {
                             if (mode == "1N")
                                 pinned<Alloc1ReleaseNSlicePool>();
                             else
                                 pinned<AllocNReleaseNSlicePool>();
                         }});
    }
}
} // namespace ct
