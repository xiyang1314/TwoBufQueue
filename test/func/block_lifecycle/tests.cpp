#include "../../warm_pool_test_support.hpp"
#include "TwoBufQueue/src/base_components/block_slice/slice_pool.hpp"
#include "TwoBufQueue/src/utils/spsc_queue.hpp"
#include "TwoBufQueue/src/base_components/block_slice/writer_block.hpp"
#include "../../block_test_support.hpp"
#include <array>
#include "TwoBufQueue/src/base_components/WarmPool/intrusive_queue.hpp"
#include <barrier>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

using namespace TwoBufQueue;
using namespace std::chrono_literals;

#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << #x << " at " << __LINE__ << '\n'; std::abort(); } } while (false)


struct SourceForTest : WriterBlock<> {
    using WriterBlock<>::WriterBlock;
    void publish(std::size_t end) {
        CHECK(state() == State::Writing && end >= write_pos() && end <= capacity());
        for (auto i = write_pos(); i < end; ++i) buf_[i] = std::byte(i);
        write_ptr_.store(buf_ + end, std::memory_order_release);
    }
    void publish_record(std::uint64_t id) {
        const auto begin = write_ptr_.load(std::memory_order_relaxed);
        const std::array<std::uint64_t, 2> record{id, ~id};
        std::memcpy(begin, record.data(), sizeof(record));
        write_ptr_.store(begin + sizeof(record), std::memory_order_release);
    }
};

template<class B> void finish(B* b) { finish_block(b); }
template<class S> void write_id(S& s, std::uint64_t id) {
    CHECK(s.size() >= 16);
    const std::array<std::uint64_t, 2> values{id, ~id};
    std::memcpy(s.data(), values.data(), 16);
}
template<class S> std::uint64_t check_id(const S& s) {
    CHECK(s.size() >= 16);
    std::array<std::uint64_t, 2> values;
    std::memcpy(values.data(), s.data(), 16);
    CHECK(values[1] == ~values[0]);
    return values[0];
}

template<class B> void states() {
    auto pool = make_block_pool<TestBlockPool<B, false>>(64, 1, 1);
    auto* b = pool.get().get();
    CHECK(!b->is_alloc_finish() && b->is_all_released() && !b->is_free());
    CHECK(pool.free_resource_count() == 0 && pool.get().get() == nullptr);
    auto s = b->allocate(8);
    CHECK(s && b->allocated_size() == 8 && !b->is_all_released());
    s.release();
    CHECK(!b->is_free() && pool.get().get() == nullptr);
    b->to_alloc_finished();
    CHECK(b->is_free() && pool.in_use_count() == 0);
    auto* next = pool.get().get();
    CHECK(next == b && next->state() == ReaderBlockState::Allocating);
    CHECK(next->allocated_size() == 0 && next->is_all_released());
    finish(next);
    CHECK(pool.all_released() && pool.in_use_count() == 0);
    CHECK(pool.free_resource_count() == 1);
}

void read_progress() {
    auto pool = make_block_pool<TestBlockPool<SourceForTest>>(64, 1, 1);
    auto* b = pool.get().get();
    b->publish(16);
    auto first = b->allocate_read_slice();
    b->publish(32);
    CHECK(b->read_pos() == 16 && !b->is_alloc_finish());
    first.release();
    finish(b);
    CHECK(b->is_all_released() && !b->is_free() && !pool.get().get());
    auto second = b->allocate_read_slice();
    CHECK(second && b->is_alloc_finish() && !b->is_all_released());
    for (unsigned i = 0; i < 16; ++i) CHECK(second.data()[i] == std::byte(16 + i));
    CHECK(!pool.get().get());
    second.release();
    CHECK(b->is_free() && pool.in_use_count() == 0);
    CHECK(pool.all_released());
}

void force_ignore() {
    BlockConfig config{.is_check_slice_release = false};
    auto pool = make_block_pool<TestBlockPool<SourceForTest>>(64, 1, 1, config);
    auto* b = pool.get().get();
    b->publish(16);
    auto old = b->allocate_read_slice();
    b->publish(32);
    finish(b);
    CHECK(!b->is_alloc_finish() && !b->is_all_released() && !b->is_free());
    b->force_recycle();
    CHECK(b->state() == WriterBlockState::Free);
    CHECK(pool.in_use_count() == 0); // Return immediately after forced completion.
    auto* next = pool.get().get();
    CHECK(next == b && next->read_pos() == 0 && next->write_pos() == 0);
    next->publish(16);
    auto current = next->allocate_read_slice();
    write_id(current, 42);
    old.release();
    CHECK(!next->is_all_released() && next->outstanding_count() == 1 && check_id(current) == 42);
    finish(next);
    CHECK(!pool.get().get());
    current.release();
    CHECK(pool.all_released());

    b = pool.get().get();
    b->publish(16);
    auto before_scan = b->allocate_read_slice();
    finish(b);
    b->force_recycle();
    before_scan.release(); // Ignore under ForceRecycle without touching old-generation counters.
    CHECK(b->outstanding_count() == 1);
    CHECK(pool.all_released());
}

void scan_skip_head() {
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(32, 4, 4);
    CHECK(pool.in_use_count() == 0);
    std::array<ReaderBlock<>*, 4> blocks{};
    std::array<MemorySlice, 4> slices{};
    for (unsigned i = 0; i < 4; ++i) { blocks[i] = pool.get().get(); slices[i] = blocks[i]->allocate(16); write_id(slices[i], i); }
    CHECK(pool.in_use_count() == 4);
    for (unsigned i = 0; i < 100; ++i) CHECK(pool.free_resource_count() == 0);
    for (auto* b : blocks) finish(b);
    slices[1].release(); slices[3].release();
    CHECK(pool.free_resource_count() == 2 && pool.in_use_count() == 2);
    CHECK(pool.in_use_count() == 2);
    auto* a = pool.get().get(); auto* b = pool.get().get();
    CHECK(a && b && a != b && (a == blocks[1] || a == blocks[3]));
    CHECK(b == blocks[1] || b == blocks[3]);
    CHECK(check_id(slices[0]) == 0 && check_id(slices[2]) == 2);
    finish(a); finish(b);
    slices[0].release(); slices[2].release();
    CHECK(pool.all_released() && pool.in_use_count() == 0);
    CHECK(pool.in_use_count() == 0);
}

template<class B> struct PointerBlock : B {
    using B::B;
    std::byte* buffer() const { return this->buf_; }
    std::byte* end() const { return this->buf_ ? this->buf_ + this->allocated_size() : nullptr; }
};

template<class Base> void pointer_cursor() {
    using Block = PointerBlock<Base>;
    Block uninitialized(BlockConfig{});
    CHECK(uninitialized.allocated_size() == 0 && uninitialized.end() == nullptr);
    auto pool = make_block_pool<TestBlockPool<Block, false>>(128, 1, 1, {.max_slices_per_block = 3});
    Block* original = nullptr;
    for (unsigned generation = 0; generation < 5; ++generation) {
        auto* b = pool.get().get();
        if (!original) original = b;
        CHECK(b == original && b->buffer() && b->end() == b->buffer());
        CHECK(b->allocated_size() == 0 && b->allocated_size() == 0);
        auto a = b->allocate(1), c = b->allocate(7, 64);
        CHECK(a && c && b->end() == c.data() + c.size() && b->allocated_size() == 71);
        CHECK(!b->allocate(58) && b->allocated_size() == 71);
        auto d = b->allocate(57);
        CHECK(d && b->end() == b->buffer() + 128 && b->allocated_size() == 128);
        CHECK(!b->allocate(1) && b->end() == b->buffer() + 128);
        a.release(); c.release(); d.release(); finish(b);
        CHECK(pool.all_released());
    }
}

void pointer_publication() {
    constexpr unsigned count = 10000;
    auto pool = make_block_pool<TestBlockPool<SourceForTest>>(count * 16, 1, 1, {.max_slices_per_block = count});
    auto* b = pool.get().get();
    std::thread writer([&] {
        for (unsigned i = 0; i < count; ++i) {
            b->publish_record(i);
            if ((i & 63) == 0) std::this_thread::yield();
        }
        finish(b);
    });
    unsigned seen = 0;
    while (seen != count) {
        auto s = b->allocate_read_slice();
        if (!s) { std::this_thread::yield(); continue; }
        CHECK(s.size() % 16 == 0);
        for (std::size_t offset = 0; offset < s.size(); offset += 16) {
            std::array<std::uint64_t, 2> record;
            std::memcpy(record.data(), s.data() + offset, 16);
            CHECK(record[0] == seen && record[1] == ~record[0]);
            ++seen;
        }
    }
    writer.join();
    CHECK(b->read_pos() == count * 16 && b->write_pos() == count * 16);
    CHECK(pool.all_released());
}

void slice_state_range() {
    auto pool = make_block_pool<TestBlockPool<SourceForTest>>(48, 1, 1);
    for (unsigned phase = 0; phase < 3; ++phase) {
        auto* b = pool.get().get();
        b->publish(48);
        if (phase >= 1) b->to_write_finished();
        if (phase >= 2) b->to_leave_writer();
        auto slice = b->allocate_read_slice();
        CHECK(slice && slice.size() == 48 && b->outstanding_count() == 1);
        CHECK(!b->allocate_read_slice()); // No duplicate registration or empty-slot consumption.
        for (unsigned i = 0; i < 48; ++i) CHECK(slice.data()[i] == std::byte(i));
        if (phase == 0) b->to_write_finished();
        if (phase < 2) b->to_leave_writer();
        CHECK(pool.free_resource_count() == 0);
        slice.release();
        CHECK(pool.all_released());
    }
}

void derived_recycle_condition() {
    struct Block : ReaderBlock<> {
        using ReaderBlock<>::ReaderBlock;
        bool returnable = false;
        void complete() noexcept { if (returnable) to_alloc_finished(); }
    };
    auto pool = make_block_pool<TestBlockPool<Block>>(16, 1, 1);
    auto* b = pool.get().get();
    b->complete();
    CHECK(pool.free_resource_count() == 0 && pool.in_use_count() == 1);
    CHECK(!pool.get().get());
    b->returnable = true;
    b->complete();
    CHECK(pool.all_released() && pool.in_use_count() == 0);
}

void intrusive_queue() {
    struct Node { Node* next = nullptr; };
    using Queue = SingleThreadQueue<Node, &Node::next>;
    static_assert(!std::is_copy_constructible_v<Queue> && !std::is_move_constructible_v<Queue>);
    Queue queue;
    std::array<Node, 64> nodes;
    CHECK(queue.empty() && queue.size() == 0 && !queue.front() && !queue.pop());
    for (unsigned i = 0; i < 100; ++i) {
        queue.push(&nodes[0]);
        CHECK(queue.front() == &nodes[0] && queue.size() == 1);
        CHECK(queue.pop() == &nodes[0] && nodes[0].next == nullptr);
        CHECK(queue.empty() && !queue.front() && !queue.pop());
    }
    SingleThreadStack<Node, &Node::next> free;
    for (auto& node : nodes) free.push(&node);
    std::deque<Node*> expected;
    std::uint32_t random = 7;
    for (unsigned i = 0; i < 20000; ++i) {
        random = random * 1664525u + 1013904223u;
        if (!free.empty() && (queue.empty() || (random & 0x10000) != 0)) {
            auto* node = free.pop();
            queue.push(node); expected.push_back(node);
        } else {
            auto* node = queue.pop();
            CHECK(node == expected.front() && node->next == nullptr);
            expected.pop_front(); free.push(node);
        }
        CHECK(queue.size() == expected.size() && queue.empty() == expected.empty());
        CHECK(queue.front() == (expected.empty() ? nullptr : expected.front()));
    }
    while (!expected.empty()) {
        auto* node = queue.pop();
        CHECK(node == expected.front() && node->next == nullptr);
        expected.pop_front(); free.push(node);
    }
    std::array<bool, 64> seen{};
    for (unsigned i = 0; i < nodes.size(); ++i) {
        auto* node = free.pop();
        CHECK(node && !seen[node - nodes.data()]);
        seen[node - nodes.data()] = true;
    }
    CHECK(free.empty() && queue.empty() && queue.size() == 0);
}

void scan_before_create() {
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(64, 1, 4);
    auto* b = pool.get().get();
    finish(b);
    CHECK(pool.get().get() == b && pool.resource_count() == 1); // Scan before growing the pool.
    finish(b);
    CHECK(pool.all_released());
    CHECK(pool.preallocate(4) && pool.resource_count() == 4);
}

template<class B> void boundaries() {
    auto pool = make_block_pool<TestBlockPool<B, false>>(128, 1, 1, {.max_slices_per_block = 3});
    auto* b = pool.get().get();
    auto a = b->allocate(1);
    auto c = b->allocate(7, 64);
    auto d = b->allocate(57);
    CHECK(a && c && d && b->allocated_size() == 128 && b->allocated_size() == 128);
    CHECK(reinterpret_cast<std::uintptr_t>(c.data()) % 64 == 0);
    CHECK(!b->allocate(1));
    a.release(); c.release(); d.release();
    CHECK(!b->allocate(1));
    finish(b);
    b = pool.get().get();
    auto a2 = b->allocate(1), c2 = b->allocate(1), d2 = b->allocate(1);
    CHECK(!b->allocate(1) && b->allocated_size() == 3); // Exhausted slice slots must not trigger growth.
    a2.release(); c2.release(); d2.release();
    CHECK(!b->allocate(1));
    finish(b);
}

void bitmap_reuse() {
    constexpr unsigned count = 1300;
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(count * 16, 1, 1, {.max_slices_per_block = count});
    for (unsigned generation = 0; generation < 8; ++generation) {
        auto* b = pool.get().get();
        std::vector<MemorySlice> slices;
        slices.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            slices.push_back(b->allocate(16));
            write_id(slices.back(), generation * count + i);
        }
        CHECK(!b->allocate(1));
        finish(b);
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < 4; ++t) threads.emplace_back([&, t] {
            for (unsigned i = t; i < count; i += 4) {
                CHECK(check_id(slices[i]) == generation * count + i);
                slices[i].release();
            }
        });
        for (auto& t : threads) t.join();
        CHECK(pool.all_released() && pool.in_use_count() == 0);
    }
}

void last_release_scan() {
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(16, 1, 1);
    for (unsigned round = 0; round < 500; ++round) {
        auto* b = pool.get().get();
        auto s = b->allocate(16);
        write_id(s, round);
        finish(b);
        std::barrier start(2);
        std::thread releaser([&, owned = std::move(s)]() mutable {
            start.arrive_and_wait();
            CHECK(check_id(owned) == round);
            owned.release();
        });
        start.arrive_and_wait();
        ReaderBlock<>* next = nullptr;
        while (!(next = pool.get().get())) std::this_thread::yield();
        auto current = next->allocate(16);
        write_id(current, round + 1000);
        releaser.join();
        CHECK(check_id(current) == round + 1000 && next->outstanding_count() == 1);
        finish(next);
        current.release();
    }
}

void many_getters() {
    constexpr unsigned producers = 4, per = 1500, total = producers * per;
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(16, 8, 16);
    std::array<std::atomic<unsigned>, total> seen{};
    std::mutex lock;
    std::condition_variable ready;
    std::deque<MemorySlice> work;
    bool done = false;
    std::vector<std::thread> readers, writers;
    for (unsigned r = 0; r < 4; ++r) readers.emplace_back([&] {
        for (;;) {
            MemorySlice s;
            { std::unique_lock guard(lock); ready.wait(guard, [&] { return done || !work.empty(); });
              if (work.empty()) return;
              s = std::move(work.front()); work.pop_front(); }
            const auto id = check_id(s);
            CHECK(id < total && seen[id].fetch_add(1) == 0);
            s.release();
        }
    });
    for (unsigned w = 0; w < producers; ++w) writers.emplace_back([&, w] {
        for (unsigned n = 0; n < per; ++n) {
            ReaderBlock<>* b;
            while (!(b = pool.get().get())) std::this_thread::yield();
            auto s = b->allocate(16);
            write_id(s, w * per + n);
            finish(b);
            { std::lock_guard guard(lock); work.push_back(std::move(s)); }
            ready.notify_one();
        }
    });
    for (auto& w : writers) w.join();
    { std::lock_guard guard(lock); done = true; }
    ready.notify_all();
    for (auto& r : readers) r.join();

    CHECK(pool.in_use_count() == 0 && pool.resource_count() <= 16);
    for (auto& n : seen) CHECK(n.load() == 1);
}

void same_block_alloc_release() {
    constexpr unsigned per = 300, threads = 4, count = per * threads;
    using Block = AllocNReleaseNReaderBlock;
    auto pool = make_block_pool<TestBlockPool<Block>>(count * 16, 1, 1, {.max_slices_per_block = count});
    auto* b = pool.get().get();
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) workers.emplace_back([&, t] {
        for (unsigned i = 0; i < per; ++i) {
            auto s = b->allocate(16);
            write_id(s, t * per + i);
            CHECK(check_id(s) == t * per + i);
            s.release();
        }
    });
    for (unsigned i = 0; i < 1000; ++i) CHECK(pool.free_resource_count() == 0);
    for (auto& w : workers) w.join();
    CHECK(b->allocated_size() == count * 16 && !b->is_alloc_finish() && b->is_all_released());
    finish(b);
    CHECK(pool.all_released());
}

void wait_timeouts() {
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(16, 1, 1);
    auto* b = pool.get().get();
    CHECK(!pool.wait_get(0ms).get() && !pool.wait_get(-1ms).get());
    const auto start = std::chrono::steady_clock::now();
    CHECK(!pool.wait_get(5ms).get());
    CHECK(std::chrono::steady_clock::now() - start >= 5ms);
    auto s = b->allocate(16);
    finish(b);
    std::thread releaser([owned = std::move(s)]() mutable { std::this_thread::sleep_for(10ms); owned.release(); });
    auto* next = pool.wait_get(2s).get();
    CHECK(next);
    releaser.join();
    finish(next);
}

void wait_multiple() {
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(16, 4, 4);
    std::array<MemorySlice, 4> slices;
    for (auto& s : slices) { auto* b = pool.get().get(); s = b->allocate(16); finish(b); }
    std::array<ReaderBlock<>*, 4> acquired{};
    std::vector<std::thread> waiters;
    std::barrier all_acquired(4);
    for (unsigned i = 0; i < 4; ++i) waiters.emplace_back([&, i] {
        acquired[i] = pool.wait_get(2s).get();
        CHECK(acquired[i]);
        all_acquired.arrive_and_wait();
        finish(acquired[i]);
    });
    std::this_thread::sleep_for(10ms);
    for (auto& s : slices) s.release();
    for (auto& t : waiters) t.join();
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = 0; j < i; ++j) CHECK(acquired[i] != acquired[j]);
    CHECK(pool.all_released());
}

template<class Pool> void slice_pool() {
    Pool pool(16, 1, 1);
    for (unsigned i = 0; i < 1000; ++i) {
        auto s = pool.allocate(16);
        CHECK(s);
        write_id(s, i);
        CHECK(check_id(s) == i);
        s.release();
        CHECK(pool.block_count() == 1 && pool.in_use_count() == 1);
    }
    pool.close(); pool.close();
    CHECK(!pool.allocate(1));
    // The final block is complete and returned immediately.
    CHECK(pool.in_use_count() == 0);
}

void close_then_release() {
    Alloc1ReleaseNSlicePool pool(64, 1, 1);
    auto first = pool.allocate(16), second = pool.allocate(16);
    pool.close();
    std::thread release([a = std::move(first), b = std::move(second)]() mutable { a.release(); b.release(); });
    release.join();
    CHECK(pool.in_use_count() == 0);
}

void notifications() {
    std::atomic<unsigned> count{0};
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(16, 1, 1);
    struct Context { TestBlockPool<ReaderBlock<>>* pool; std::atomic<unsigned>* count; } context{&pool, &count};
    pool.set_release_callback([](void* p) noexcept {
        auto& c = *static_cast<Context*>(p);
        CHECK(c.pool->all_released());
        ++*c.count;
    }, &context);
    auto* b = pool.get().get(); auto s = b->allocate(16);
    finish(b); s.release();
    CHECK(count.load() == 1 && pool.in_use_count() == 0 && b->is_free());
    CHECK(pool.all_released() && count.load() == 1);
    b = pool.get().get(); finish(b);
    b = pool.get().get();
    CHECK(count.load() == 2);
    finish(b);
    pool.set_release_callback(nullptr, nullptr);
}

void destructor_scan() {
    std::atomic<unsigned> count{0};
    {
        auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(16, 1, 1);
        pool.set_release_callback([](void* p) noexcept { ++*static_cast<std::atomic<unsigned>*>(p); }, &count);
        auto* b = pool.get().get(); finish(b);
        CHECK(pool.in_use_count() == 0);
    }
    CHECK(count.load() == 1);
}

void death(std::string_view name) {
    std::set_terminate([] { std::_Exit(91); });
    BlockDiagnostics<WriterBlock<>::StorageBlock>::set_error_handler([](const auto&) noexcept { std::_Exit(91); });
    if (name == "allocate_finished") {
        auto p = make_block_pool<TestBlockPool<ReaderBlock<>>>(64, 1, 1);
        auto* r = p.get().get(); r->to_alloc_finished();
        auto bad = r->allocate(1); (void)bad;
        std::_Exit(92);
    }
    auto pool = make_block_pool<TestBlockPool<SourceForTest>>(64, 1, 1);
    auto* b = pool.get().get();
    if (name == "force_writing") b->force_recycle();
    if (name == "slice_free") {
        finish(b);
        CHECK(pool.all_released());
        auto invalid = b->allocate_read_slice(); (void)invalid;
    }
    if (name == "leave_writing") b->to_leave_writer();
    if (name == "destructor_writing") return;
    b->publish(16);
    auto s = b->allocate_read_slice();
    if (name == "destructor_slice") {
        auto* held = new MemorySlice(std::move(s)); (void)held;
        finish(b); return;
    }
    b->publish(32);
    if (name == "slice_bounds") {
        b->write_ptr_.store(reinterpret_cast<std::byte*>(reinterpret_cast<std::uintptr_t>(b->buf_) + 65));
        auto bad = b->allocate_read_slice(); (void)bad;
    }
    b->to_write_finished();
    if (name == "force_finished") b->force_recycle();
    if (name == "finish_twice") b->to_write_finished();
    b->to_leave_writer();
    if (name == "leave_twice") b->to_leave_writer();
    if (name == "destructor_unread") { s.release(); return; }
    if (name == "release_twice") { s.release(); s.release(); }
    b->force_recycle();
    if (name == "slice_forced") { auto invalid = b->allocate_read_slice(); (void)invalid; }
    if (name == "force_twice") b->force_recycle();
    if (name == "release_forced") s.release();
    CHECK(pool.all_released());
    if (name == "force_free") b->force_recycle();
    auto* next = pool.get().get(); next->publish(16);
    auto current = next->allocate_read_slice();
    if (name == "release_stale") s.release();
    std::_Exit(92);
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string_view name = argv[1];
    if (name.starts_with("death_")) { death(name.substr(6)); return 92; }
    if (name == "state_single") states<Alloc1Release1ReaderBlock>();
    else if (name == "state_1n") states<Alloc1ReleaseNReaderBlock>();
    else if (name == "state_nn") states<AllocNReleaseNReaderBlock>();
    else if (name == "read_progress") read_progress();
    else if (name == "pointer_single") pointer_cursor<Alloc1Release1ReaderBlock>();
    else if (name == "pointer_1n") pointer_cursor<Alloc1ReleaseNReaderBlock>();
    else if (name == "pointer_nn") pointer_cursor<AllocNReleaseNReaderBlock>();
    else if (name == "pointer_publication") pointer_publication();
    else if (name == "slice_state_range") slice_state_range();
    else if (name == "derived_recycle_condition") derived_recycle_condition();
    else if (name == "intrusive_queue") intrusive_queue();
    else if (name == "force_ignore") force_ignore();
    else if (name == "scan_skip_head") scan_skip_head();
    else if (name == "scan_before_create") scan_before_create();
    else if (name == "bounds_single") boundaries<Alloc1Release1ReaderBlock>();
    else if (name == "bounds_1n") boundaries<Alloc1ReleaseNReaderBlock>();
    else if (name == "bounds_nn") boundaries<AllocNReleaseNReaderBlock>();
    else if (name == "bitmap_reuse") bitmap_reuse();
    else if (name == "last_release_scan") last_release_scan();
    else if (name == "many_getters") many_getters();
    else if (name == "same_block_alloc_release") same_block_alloc_release();
    else if (name == "wait_timeouts") wait_timeouts();
    else if (name == "wait_multiple") wait_multiple();
    else if (name == "slice_pool_single") slice_pool<Alloc1Release1SlicePool>();
    else if (name == "slice_pool_1n") slice_pool<Alloc1ReleaseNSlicePool>();
    else if (name == "slice_pool_nn") slice_pool<AllocNReleaseNSlicePool>();
    else if (name == "close_then_release") close_then_release();
    else if (name == "notifications") notifications();
    else if (name == "destructor_scan") destructor_scan();
    else { CHECK(false); }
    std::cout << "PASS " << name << '\n';
}
