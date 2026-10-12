#include "../../../support/joining_thread.hpp"
#include "../../warm_pool_test_support.hpp"
#include "../concurrent/common.hpp"
#include <array>
#include <memory>

using namespace ct;

template<class S> void null_pool() {
    bool caught = false;
    try { S slices(nullptr); }
    catch (const std::invalid_argument&) { caught = true; }
    CHECK(caught);
}

template<class S> void lifetime() {
    auto pool = make_block_pool<typename S::Pool>(64, 0, 1);
    typename S::Slice held;
    {
        S slices(&pool);
        held = slices.allocate(32);
        CHECK(held);
        std::memset(held.data(), 0x57, held.size());
    }
    CHECK(pool.in_use_count() == 1 && !pool.all_released());
    for (std::size_t i = 0; i < held.size(); ++i) CHECK(held.data()[i] == std::byte{0x57});
    held.release();
    CHECK(pool.all_released() && pool.all_released());
    {
        S slices(&pool);
        auto next = slices.allocate(64);
        CHECK(next && pool.resource_count() == 1);
        next.release();
    }
    CHECK(pool.all_released());
}

template<class S> void shared() {
    auto pool = make_block_pool<typename S::Pool>(32, 0, 2, {.max_slices_per_block = 2});
    S a(&pool), b(&pool);
    auto a1 = a.allocate(16), b1 = b.allocate(16), a2 = a.allocate(16);
    CHECK(a1 && b1 && a2);
    std::memset(b1.data(), 0x39, b1.size());
    CHECK(a.block_count() == 2 && b.in_use_count() == 2);
    CHECK(!a.allocate(16)); // Both physical blocks are pinned.
    CHECK(!a.all_released() && !b.all_released());
    a1.release(); a2.release();
    auto again = a.allocate(16);
    CHECK(again);
    a.close(); a.close();
    CHECK(!a.allocate(1));
    auto b2 = b.allocate(16);
    CHECK(b2); // Closing a must not close b or its current block.
    again.release();
    CHECK(!a.all_released()); // Statistics and queries concern the whole pool.
    for (std::size_t i = 0; i < b1.size(); ++i) CHECK(b1.data()[i] == std::byte{0x39});
    b1.release(); b2.release(); b.close();
    CHECK(a.all_released() && b.all_released());
    CHECK(pool.all_released());
}

void minimum_count() {
    using P = Alloc1ReleaseNSlicePool::Pool;
    bool caught = false;
    try { auto bad = make_block_pool<P>(64, 3, 2); }
    catch (const std::invalid_argument& e) {
        caught = std::string_view(e.what()).find("min_count") != std::string_view::npos;
    }
    CHECK(caught);
    auto lazy = make_block_pool<P>(64, 0, 1);
    CHECK(lazy.resource_count() == 0);
    auto pool = make_block_pool<P>(64, 2, 4, {}, 0);
    CHECK(pool.resource_count() == 2 && pool.free_resource_count() == 2);
    std::array<ReaderBlock<>*, 4> blocks;
    for (auto& block : blocks) { block = pool.get().get(); CHECK(block); }
    CHECK(pool.resource_count() == 4);
    for (auto* block : blocks) block->to_alloc_finished();
    CHECK(pool.all_released());
    CHECK(pool.resource_count() == 2 && pool.free_resource_count() == 2);
    Alloc1ReleaseNSlicePool owned(64, 2, 4, {}, 0);
    CHECK(owned.block_count() == 2);
}

void callback_lifetime() {
    using S = Alloc1ReleaseNSlicePool;
    auto pool = make_block_pool<S::Pool>(64, 0, 1);
    std::atomic<unsigned> calls{0};
    S::Slice held;
    {
        S slices(&pool);
        slices.set_release_callback([](void* p) noexcept {
            static_cast<std::atomic<unsigned>*>(p)->fetch_add(1, std::memory_order_relaxed);
        }, &calls);
        held = slices.allocate(16);
    }
    held.release();
    CHECK(pool.all_released() && calls.load() == 1);
    auto* block = pool.get().get(); CHECK(block);
    block->to_alloc_finished();
    CHECK(pool.all_released() && calls.load() == 2);
    pool.set_release_callback(nullptr, nullptr);
}

template<class S, unsigned writers_per_adapter> void concurrent_shared() {
    constexpr unsigned adapters = 4, writers = adapters * writers_per_adapter;
    constexpr unsigned each = 500, total = (writers + 1) * each;
    auto pool = make_block_pool<typename S::Pool>(128, 2, 12, {.max_slices_per_block = 4}, 4);
    std::array<std::unique_ptr<S>, adapters> slices;
    for (auto& s : slices) s = std::make_unique<S>(&pool);
    Channel<typename S::Slice> queue(32);
    std::array<std::atomic<unsigned>, total> seen{};
    const auto deadline = Clock::now() + 45s;
    std::atomic<bool> stop_scan{false};
    auto retry = [&] { CHECK(Clock::now() < deadline); std::this_thread::yield(); };
    auto publish_slice = [&](typename S::Slice& slice, unsigned id) {
        const std::array<std::uint64_t, 2> record{id, ~std::uint64_t{id}};
        std::memcpy(slice.data(), record.data(), 16);
        while (!queue.push(slice)) retry();
    };
    brq_support::JoiningThread scanner([&] {
        while (!stop_scan.load(std::memory_order_acquire)) {

            const auto count = pool.resource_count();
            CHECK(count >= 2 && count <= 12);
            retry();
        }
    });
    std::vector<brq_support::JoiningThread> consumers;
    for (unsigned i = 0; i < 4; ++i) consumers.emplace_back([&] {
        while (auto slice = queue.pop()) {
            CHECK(slice->size() == 16);
            std::array<std::uint64_t, 2> record;
            std::memcpy(record.data(), slice->data(), 16);
            CHECK(record[0] < total && record[1] == ~record[0]);
            CHECK(seen[record[0]].fetch_add(1, std::memory_order_relaxed) == 0);
            std::this_thread::yield();
            std::array<std::uint64_t, 2> retained;
            std::memcpy(retained.data(), slice->data(), 16);
            CHECK(record == retained);
            slice->release();
        }
    });
    std::vector<brq_support::JoiningThread> producers;
    for (unsigned id = 0; id < writers; ++id) producers.emplace_back([&, id] {
        for (unsigned i = 0; i < each; ++i) {
            typename S::Slice slice;
            while (!(slice = slices[id / writers_per_adapter]->allocate(16))) retry();
            publish_slice(slice, id * each + i);
        }
    });
    // A direct caller must share the same pool lock with all SlicePool adapters.
    producers.emplace_back([&] {
        for (unsigned i = 0; i < each; ++i) {
            decltype(pool.get().get()) block = nullptr;
            while (!(block = pool.get().get())) retry();
            auto slice = block->allocate(16); CHECK(slice);
            block->to_alloc_finished();
            publish_slice(slice, writers * each + i);
        }
    });
    for (auto& thread : producers) thread.join();
    for (auto& s : slices) s->close();
    queue.close();
    for (auto& thread : consumers) thread.join();
    stop_scan.store(true, std::memory_order_release); scanner.join();
    CHECK(pool.all_released());

    CHECK(pool.in_use_count() == 0);
    for (const auto& count : seen) CHECK(count.load() == 1);
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string_view name = argv[1];
    if (name == "null_single") null_pool<Alloc1Release1SlicePool>();
    else if (name == "null_1n") null_pool<Alloc1ReleaseNSlicePool>();
    else if (name == "null_nn") null_pool<AllocNReleaseNSlicePool>();
    else if (name == "lifetime_single") lifetime<Alloc1Release1SlicePool>();
    else if (name == "lifetime_1n") lifetime<Alloc1ReleaseNSlicePool>();
    else if (name == "lifetime_nn") lifetime<AllocNReleaseNSlicePool>();
    else if (name == "shared_single") shared<Alloc1Release1SlicePool>();
    else if (name == "shared_1n") shared<Alloc1ReleaseNSlicePool>();
    else if (name == "shared_nn") shared<AllocNReleaseNSlicePool>();
    else if (name == "min_count") minimum_count();
    else if (name == "callback") callback_lifetime();
    else if (name == "concurrent_1n") concurrent_shared<Alloc1ReleaseNSlicePool, 1>();
    else if (name == "concurrent_nn") concurrent_shared<AllocNReleaseNSlicePool, 2>();
    else CHECK(false);
    std::cout << "PASS " << name << '\n';
}
