#include "../../../support/joining_thread.hpp"
#include "../../warm_pool_test_support.hpp"
#include "../concurrent/common.hpp"
#include "../../block_test_support.hpp"
#include <concepts>

using namespace ct;
static_assert(PoolResource<ReaderBlock<>> && PoolResource<WriterBlock<>>);

template<class T> concept HasState = requires(const T& b) { typename T::State; b.state(); };
template<class T> concept HasRecycle = requires(T& b) { b.force_recycle(); b.is_free(); };
template<class T> concept HasName = requires { &T::state_name; };
static_assert(!HasState<BasicBlock<>> && !HasRecycle<BasicBlock<>> && !HasName<BasicBlock<>>);
static_assert(!std::same_as<ReaderBlock<>::State, WriterBlock<>::State>);
static_assert(!BasicBlock<false, false>::concurrent);
static_assert(BasicBlock<true, false>::concurrent);
static_assert(std::same_as<ReaderBlock<true, false>::Slice, WriterBlock<true, false>::Slice>);
static_assert(std::same_as<decltype(WriterBlock<false, false>::write_ptr_), PlainValue<std::byte*>>);
static_assert(std::same_as<decltype(WriterBlock<true, false>::write_ptr_), std::atomic<std::byte*>>);

template<class B> static void publish(B* b, std::uint64_t first, std::uint64_t end) {
    for (auto id = first; id < end; ++id) {
        const std::array<std::uint64_t, 2> record{id, ~id};
        std::memcpy(b->buf_ + id * 16, record.data(), 16);
    }
    b->write_ptr_.store(b->buf_ + end * 16, std::memory_order_release);
}
template<class S> static void verify(const S& slice, std::uint64_t first) {
    CHECK(slice && slice.size() % 16 == 0);
    for (std::size_t offset = 0; offset < slice.size(); offset += 16) {
        std::array<std::uint64_t, 2> record;
        std::memcpy(record.data(), slice.data() + offset, 16);
        CHECK(record[0] == first + offset / 16 && record[1] == ~record[0]);
    }
}
template<bool A, bool R> static void writer_mode() {
    using B = WriterBlock<A, R>;
    auto pool = make_block_pool<TestBlockPool<B, false>>(128, 1, 1, {.max_slices_per_block = 2});
    for (unsigned round = 0; round < 20; ++round) {
        auto* b = pool.get().get();
        CHECK(b->is_free() == false && b->state() == WriterBlockState::Writing);
        CHECK(b->read_pos() == 0 && b->write_ptr_.load() == b->buf_);
        CHECK(!b->allocate_read_slice());
        publish(b, 0, 1);
        auto first = b->allocate_read_slice();
        publish(b, 1, 8);
        finish_block(b);
        first.release();
        CHECK(!b->is_free() && !pool.get().get());
        auto tail = b->allocate_read_slice();
        verify(tail, 1);
        CHECK(b->is_alloc_finish() && !pool.get().get());
        tail.release();
        CHECK(pool.all_released());
    }
    CHECK(std::string_view(B::state_name(WriterBlockState::LeaveWriter)) == "LeaveWriter");
    CHECK(std::string_view(B::state_name(static_cast<WriterBlockState>(255))) == "Invalid");
}

static void reader_10() {
    using B = ReaderBlock<true, false>;
    constexpr unsigned workers = 4, each = 1000, total = workers * each;
    auto pool = make_block_pool<TestBlockPool<B>>(total * 16, 1, 1, {.max_slices_per_block = total});
    auto* b = pool.get().get();
    Channel<B::Slice> queue(16);
    std::array<bool, total> seen{};
    std::atomic<unsigned> released{0};
    brq_support::JoiningThread consumer([&] {
        while (auto slice = queue.pop()) {
            std::array<std::uint64_t, 2> record;
            std::memcpy(record.data(), slice->data(), 16);
            CHECK(record[0] < total && record[1] == ~record[0] && !seen[record[0]]);
            seen[record[0]] = true;
            slice->release();
            released.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::vector<brq_support::JoiningThread> allocators;
    for (unsigned w = 0; w < workers; ++w) allocators.emplace_back([&, w] {
        for (unsigned i = 0; i < each; ++i) {
            auto slice = b->allocate(16);
            CHECK(slice);
            const std::uint64_t id = w * each + i;
            const std::array<std::uint64_t, 2> record{id, ~id};
            std::memcpy(slice.data(), record.data(), 16);
            while (!queue.push(slice)) std::this_thread::yield();
        }
    });
    for (auto& thread : allocators) thread.join();
    b->to_alloc_finished(); queue.close(); consumer.join();
    CHECK(released.load() == total && b->allocated_size() == total * 16);
    for (auto value : seen) CHECK(value);
    CHECK(pool.all_released());
}

template<bool release_many> static void writer_parallel() {
    using B = WriterBlock<true, release_many>;
    constexpr unsigned readers = 4, records = 1024;
    for (unsigned round = 0; round < 12; ++round) {
        auto pool = make_block_pool<TestBlockPool<B>>(records * 16, 1, 1, {.max_slices_per_block = records});
        auto* b = pool.get().get();
        publish(b, 0, 1);
        auto anchor = b->allocate_read_slice();
        // 分配者持有裸块指针期间保留一个切片，避免其空扫描与整块复用竞争。
        Channel<typename B::Slice> queue(8);
        std::array<std::atomic<unsigned>, records> seen{};
        std::atomic<bool> stop_scan{false};
        brq_support::JoiningThread scanner([&] {
            while (!stop_scan.load(std::memory_order_acquire)) {
                CHECK(pool.free_resource_count() == 0);
                std::this_thread::yield();
            }
        });
        std::vector<brq_support::JoiningThread> releasers;
        for (unsigned t = 0; t < (release_many ? 4u : 1u); ++t) releasers.emplace_back([&] {
            while (auto slice = queue.pop()) {
                const auto first = static_cast<std::size_t>(slice->data() - b->buf_) / 16;
                CHECK(first < records && slice->size() % 16 == 0);
                verify(*slice, first);
                std::this_thread::yield();
                verify(*slice, first);
                for (std::size_t i = 0; i < slice->size() / 16; ++i)
                    CHECK(seen[first + i].fetch_add(1, std::memory_order_relaxed) == 0);
                slice->release();
            }
        });
        brq_support::JoiningThread writer([&] {
            for (unsigned i = 1; i < records; ++i) {
                publish(b, i, i + 1);
                if ((i & 3u) == 0) std::this_thread::yield();
            }
            finish_block(b);
        });
        std::vector<brq_support::JoiningThread> allocators;
        for (unsigned t = 0; t < readers; ++t) allocators.emplace_back([&] {
            while (b->read_pos() != records * 16) {
                const auto snapshot = b->read_snapshot();
                CHECK(snapshot.read_size <= snapshot.published_size);
                auto slice = b->allocate_read_slice();
                if (!slice) { std::this_thread::yield(); continue; }
                while (!queue.push(slice)) std::this_thread::yield();
            }
        });
        writer.join();
        for (auto& thread : allocators) thread.join();
        verify(anchor, 0);
        CHECK(pool.free_resource_count() == 0);
        // 单释放者策略下，anchor 也交给同一个释放者。
        stop_scan.store(true, std::memory_order_release); scanner.join();
        while (!queue.push(anchor)) std::this_thread::yield();
        queue.close();
        for (auto& thread : releasers) thread.join();
        for (const auto& count : seen) CHECK(count.load() == 1);
        CHECK(pool.all_released());
    }
}

template<class B> static typename B::Slice allocate_one(B* b) {
    if constexpr (requires { b->allocate(16); }) return b->allocate(16);
    else {
        const auto begin = b->write_pos() / 16;
        publish(b, begin, begin + 1);
        return b->allocate_read_slice();
    }
}
template<class B> static void force_case() {
    auto pool = make_block_pool<TestBlockPool<B>>(64, 1, 1, {.is_check_slice_release = false});
    auto* b = pool.get().get();
    auto old1 = allocate_one(b), old2 = allocate_one(b);
    finish_block(b); b->force_recycle();
    old1.release();
    CHECK(b->outstanding_count() == 2);
    CHECK(pool.all_released());
    auto* next = pool.get().get(); CHECK(next == b);
    auto fresh = allocate_one(next);
    std::memset(fresh.data(), 0x71, fresh.size());
    old2.release();
    CHECK(next->outstanding_count() == 1);
    for (std::size_t i = 0; i < fresh.size(); ++i) CHECK(fresh.data()[i] == std::byte{0x71});
    finish_block(next); fresh.release();
    CHECK(pool.all_released());
}

template<class B> static void death_state() {
    using P = TestBlockPool<B>;
    TestDiagnostics<P>::set_error_handler([](const typename TestDiagnostics<P>::ErrorInfo& info) noexcept {
        const char* expected;
        typename B::State value;
        if constexpr (requires(B& b) { b.to_alloc_finished(); }) {
            // 已归还的 Loan 不能安全读取旧资源状态。
            if (!info.has_actual_state && std::strcmp(info.reason, "expected Allocating block") == 0) {
                std::fputs("EXPECTED_STATE unavailable\n", stderr);
                std::fflush(stderr); std::_Exit(91);
            }
            expected = "Free"; value = ReaderBlockState::Free;
        } else {
            expected = "Writing"; value = WriterBlockState::Writing;
        }
        if (info.has_actual_state && info.actual_state == static_cast<std::uint8_t>(value) &&
            std::strcmp(info.actual_state_name, expected) == 0) {
            std::fprintf(stderr, "EXPECTED_STATE %s\n", info.actual_state_name);
            std::fflush(stderr); std::_Exit(91);
        }
        std::_Exit(92);
    });
    auto pool = make_block_pool<P>(64, 1, 1);
    auto* b = pool.get().get();
    if constexpr (requires { b->to_alloc_finished(); }) {
        b->to_alloc_finished(); b->to_alloc_finished();
    } else b->to_leave_writer();
    std::_Exit(92);
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string_view name = argv[1];
    if (name == "interfaces") {}
    else if (name == "writer_00") writer_mode<false, false>();
    else if (name == "writer_01") writer_mode<false, true>();
    else if (name == "writer_10") writer_mode<true, false>();
    else if (name == "writer_11") writer_mode<true, true>();
    else if (name == "reader_10") reader_10();
    else if (name == "writer_parallel_10") writer_parallel<false>();
    else if (name == "writer_parallel_11") writer_parallel<true>();
    else if (name == "force_writer") force_case<WriterBlock<>>();
    else if (name == "force_reader") force_case<ReaderBlock<>>();
    else if (name == "death_writer_state") death_state<WriterBlock<>>();
    else if (name == "death_reader_state") death_state<ReaderBlock<>>();
    else CHECK(false);
    std::cout << "PASS " << name << '\n';
}
