#include "../../../support/joining_thread.hpp"
#include "../../warm_pool_test_support.hpp"
#include "../support.hpp"
#include <concepts>

using namespace ft;
using CommonBlock = WriterBlock<>::StorageBlock;

template<class T> concept HasAllocate = requires(T& b) { b.allocate(1); };
template<class T> concept HasReadSlice = requires(T& b) { b.allocate_read_slice(); };
template<class T> concept HasWritePointer = requires(T& b) { b.write_ptr_; };
template<class T> concept HasReadPosition = requires(T& b) { b.read_pos(); };
template<class T> concept HasExplicitRange = requires(T& b) { b.make_slice(0, 1); };
template<class T> concept HasReadArgument = requires(T& b) { b.allocate_read_slice(1); };

static_assert(std::same_as<ReaderBlock<>::StorageBlock, CommonBlock>);
static_assert(std::same_as<ReaderBlock<>::Slice, MemorySlice>);
static_assert(std::same_as<WriterBlock<>::Slice, MemorySlice>);
static_assert(!HasAllocate<CommonBlock> && !HasReadSlice<CommonBlock>);
static_assert(!HasWritePointer<CommonBlock> && !HasReadPosition<CommonBlock>);
static_assert(!HasAllocate<WriterBlock<>> && HasReadSlice<WriterBlock<>>);
static_assert(HasAllocate<ReaderBlock<>> && !HasReadSlice<ReaderBlock<>>);
static_assert(!HasWritePointer<ReaderBlock<>> && !HasReadPosition<ReaderBlock<>>);
static_assert(!HasExplicitRange<WriterBlock<>> && !HasReadArgument<WriterBlock<>>);
static_assert(!std::is_polymorphic_v<CommonBlock> && !std::is_polymorphic_v<WriterBlock<>>);

static void finish(WriterBlock<>* b) { b->to_write_finished(); b->to_leave_writer(); }
static void publish(WriterBlock<>* b, std::size_t begin, std::size_t end, unsigned seed) {
    for (auto i = begin; i < end; ++i) b->buf_[i] = std::byte((i + seed) & 255);
    b->write_ptr_.store(b->buf_ + end, std::memory_order_release);
}
static void verify(const std::byte* p, std::size_t size, std::size_t begin, unsigned seed) {
    for (std::size_t i = 0; i < size; ++i) CHECK(p[i] == std::byte((begin + i + seed) & 255));
}

static void mixed_batch() {
    auto source = make_block_pool<TestBlockPool<WriterBlock<>>>(128, 1, 1);
    auto saved = make_block_pool<TestBlockPool<ReaderBlock<>>>(128, 1, 1);
    auto* w = source.get().get();
    auto* r = saved.get().get();
    publish(w, 0, 64, 17);
    auto direct = w->allocate_read_slice();
    auto buffered = r->allocate(64);
    std::memcpy(buffered.data(), direct.data(), direct.size());
    CHECK(source.owns(w->loan()) && !source.owns(r->loan()) && saved.owns(r->loan()) && !saved.owns(w->loan()));
    finish(w); r->to_alloc_finished();
    CHECK(source.free_resource_count() == 0 && saved.free_resource_count() == 0);
    brq_support::JoiningThread consumer([direct = std::move(direct), buffered = std::move(buffered), &source, &saved]() mutable {
        const std::array<DataFrame, 2> frames{{{1, direct.data(), direct.size(), 0},
                                              {2, buffered.data(), buffered.size(), 0}}};
        const DataBatch batch{frames, direct.size() + buffered.size()};
        for (const auto& frame : batch.frames) verify(frame.data, frame.size, 0, 17);
        buffered.release();
        CHECK(saved.all_released() && source.free_resource_count() == 0);
        verify(direct.data(), direct.size(), 0, 17);
        direct.release();
        CHECK(source.all_released());
    });
    consumer.join();
    CHECK(source.in_use_count() == 0 && saved.in_use_count() == 0);
}

template<class B> static void reader() {
    auto pool = make_block_pool<TestBlockPool<B, false>>(128, 1, 1, {.max_slices_per_block = 3});
    for (unsigned round = 0; round < 32; ++round) {
        auto* b = pool.get().get();
        CHECK(b && b->state() == ReaderBlockState::Allocating && b->allocated_size() == 0);
        auto first = b->allocate(3);
        CHECK(first && !b->allocate(129) && b->allocated_size() == 3);
        auto second = b->allocate(7, 64);
        CHECK(second && second.data() == first.data() + 64 && b->allocated_size() == 71);
        std::memset(first.data(), 0x17, first.size());
        auto tail = b->allow_without_lock(57);
        CHECK(tail && b->allocated_size() == 128 && !b->allocate(1));
        second.release(); tail.release();
        CHECK(!b->is_alloc_finish() && pool.free_resource_count() == 0);
        b->to_alloc_finished();
        CHECK(b->is_alloc_finish() && !b->is_free() && !pool.get().get());
        for (std::size_t i = 0; i < first.size(); ++i) CHECK(first.data()[i] == std::byte{0x17});
        first.release();
        CHECK(pool.all_released());
    }
}

static void writer_publish() {
    auto pool = make_block_pool<TestBlockPool<WriterBlock<>>>(128, 1, 1, {.max_slices_per_block = 2});
    auto* b = pool.get().get();
    CHECK(!b->allocate_read_slice() && b->outstanding_count() == 0);
    std::memset(b->buf_, 0x33, 128); // Unpublished data must not produce slices.
    CHECK(!b->allocate_read_slice() && b->read_pos() == 0);
    publish(b, 0, 17, 3);
    auto first = b->allocate_read_slice();
    CHECK(first && first.size() == 17 && b->read_pos() == 17);
    for (unsigned i = 0; i < 100; ++i) CHECK(!b->allocate_read_slice());
    CHECK(b->outstanding_count() == 1);
    publish(b, 17, 128, 3);
    b->to_write_finished();
    first.release();
    CHECK(!pool.get().get()); // The completion record has not left Writer yet.
    b->to_leave_writer();
    CHECK(!pool.get().get()); // Dequeued, but a tail remains unregistered.
    auto tail = b->allocate_read_slice();
    CHECK(tail && tail.size() == 111 && b->is_alloc_finish() && !pool.get().get());
    verify(tail.data(), tail.size(), 17, 3);
    tail.release();
    CHECK(pool.all_released());
}

static void writer_trim_restore() {
    auto pool = make_block_pool<TestBlockPool<WriterBlock<>>>(128, 0, 1, {}, 0);
    TestBlockPool<WriterBlock<>>::Loan old;
    for (unsigned round = 0; round < 128; ++round) {
        auto loan = pool.get();
        auto* b = loan.get();
        CHECK(loan && !pool.is_valid(old));
        CHECK(b->buf_ && b->write_ptr_.load() == b->buf_);
        CHECK(b->read_pos() == 0 && b->write_pos() == 0);
        publish(b, 0, 128, round);
        auto slice = b->allocate_read_slice();
        finish(b);
        verify(slice.data(), slice.size(), 0, round);
        slice.release();
        CHECK(pool.all_released() && pool.resource_count() == 0);
        CHECK(!pool.is_valid(loan) && pool.will_dispose_count() == 1);
        old = loan; // After return retain only the ticket; do not dereference an object that may have been destroyed.
    }
}

static void writer_scan_release() {
    constexpr std::size_t bytes = 4096;
    auto pool = make_block_pool<TestBlockPool<WriterBlock<>>>(bytes, 1, 1, {.max_slices_per_block = bytes / 16});
    for (unsigned round = 0; round < 80; ++round) {
        auto* b = pool.get().get();
        SpscQueue<MemorySlice> queue(8);
        std::barrier start(4);
        brq_support::JoiningThread writer([&] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < bytes; i += 16) {
                publish(b, i, i + 16, round);
                if ((i & 63) == 0) std::this_thread::yield();
            }
            finish(b);
        });
        brq_support::JoiningThread reader([&] {
            start.arrive_and_wait();
            std::size_t read = 0;
            while (read != bytes) {
                auto slice = b->allocate_read_slice();
                if (!slice) { std::this_thread::yield(); continue; }
                read += slice.size();
                while (!queue.try_push(std::move(slice))) std::this_thread::yield();
            }
            // Do not access b after handing off the final handle; the pool may reuse it immediately.
        });
        brq_support::JoiningThread consumer([&] {
            start.arrive_and_wait();
            std::size_t read = 0;
            while (read != bytes) {
                auto* slot = queue.front();
                if (!slot) { std::this_thread::yield(); continue; }
                auto slice = std::move(*slot); queue.pop();
                verify(slice.data(), slice.size(), read, round);
                std::this_thread::yield();
                verify(slice.data(), slice.size(), read, round);
                read += slice.size();
                slice.release();
            }
        });
        start.arrive_and_wait();
        WriterBlock<>* reused = nullptr;
        while (!(reused = pool.get().get())) std::this_thread::yield();
        CHECK(reused == b && reused->read_pos() == 0 && reused->write_pos() == 0);
        std::memset(reused->buf_, 0xfa, bytes); // Premature reuse would corrupt consumer-side data verification.
        writer.join(); reader.join(); consumer.join();
        finish(reused);
        CHECK(pool.all_released());
    }
}

static void reader_finish_with_held_slice() {
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(64, 1, 1);
    auto* b = pool.get().get();
    auto slice = b->allocate(64);
    b->to_alloc_finished();
    CHECK(!pool.get().get());
    for (unsigned i = 0; i < 64; ++i) slice.data()[i] = std::byte(i);
    brq_support::JoiningThread consumer([held = std::move(slice)]() mutable {
        verify(held.data(), held.size(), 0, 0);
        held.release();
    });
    consumer.join();
    CHECK(pool.all_released());
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string_view name = argv[1];
    if (name == "interfaces") {}
    else if (name == "mixed_batch") mixed_batch();
    else if (name == "reader_single") reader<Alloc1Release1ReaderBlock>();
    else if (name == "reader_1n") reader<Alloc1ReleaseNReaderBlock>();
    else if (name == "reader_nn") reader<AllocNReleaseNReaderBlock>();
    else if (name == "writer_publish") writer_publish();
    else if (name == "writer_trim_restore") writer_trim_restore();
    else if (name == "writer_scan_release") writer_scan_release();
    else if (name == "reader_finish_with_held_slice") reader_finish_with_held_slice();
    else CHECK(false);
    std::cout << "PASS " << name << '\n';
}
