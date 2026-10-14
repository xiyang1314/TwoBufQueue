#include "TwoBufQueue/src/data/spsc_write_block.hpp"
#include "TwoBufQueue/src/data/spsc_read_block.hpp"
#include "TwoBufQueue/src/base_components/WarmPool/warm_pool.hpp"
#include "TwoBufQueue/src/utils/spsc_queue.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>
#include <type_traits>

using namespace TwoBufQueue;
#define CHECK(x) do { if (!(x)) { std::cerr << #x << ':' << __LINE__ << '\n'; std::abort(); } } while (false)

template<class T> using Pool = WarmPool<T, true, SpscBlockFactory<T>>;
template<class T> concept HasSlice = requires { typename T::Slice; };
template<class T> concept HasAllocate = requires(T& block) { block.allocate(1); };
static_assert(PoolResource<SpscBasicBlock> && PoolResource<SpscWriteBlock> && PoolResource<SpscReadBlock>);
static_assert(!HasSlice<SpscWriteBlock> && !HasSlice<SpscReadBlock>);
static_assert(!HasAllocate<SpscWriteBlock> && !HasAllocate<SpscReadBlock>);
static_assert(!std::is_copy_constructible_v<SpscBasicBlock> && !std::is_move_constructible_v<SpscBasicBlock>);

void storage() {
    for (auto invalid : {std::size_t{0}, (std::numeric_limits<std::size_t>::max)()}) {
        bool rejected = false;
        try { SpscBasicBlock bad(invalid); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        rejected = false;
        try { SpscBlockFactory<SpscBasicBlock> bad(invalid); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
    Pool<SpscBasicBlock> pool(1, 1, SpscBlockFactory<SpscBasicBlock>{65});
    auto first = pool.get();
    CHECK(first && first->capacity() == 65);
    CHECK(first->buf_ == first->data());
    CHECK(reinterpret_cast<std::uintptr_t>(first->data()) % 64 == 0);
    first->data()[64] = std::byte{42};
    auto* address = first->data();
    CHECK(first->finish() && !pool.is_valid(first));
    auto next = pool.get();
    CHECK(next && next->data() == address && next->data()[64] == std::byte{42});
    CHECK(next.generation() != first.generation() && !first.recycle());
    CHECK(next->finish());
}

// Writer bypasses commit_write and updates buf_ / write_ptr_ directly; readers must see the same progress.
void writer_fields() {
    using Block = SpscWriteBlock;
    using State = Block::State;
    Block unbound(8);
    CHECK(unbound.is_free() && unbound.write_pos() == 0);
    CHECK(!unbound.reset(0) && unbound.is_free());
    Pool<Block> pool(1, 1, SpscBlockFactory<Block>{8});
    auto loan = pool.get(); auto* block = loan.get();
    CHECK(block->state() == State::Writing && !block->is_free());
    CHECK(block->sequence_ == loan.generation());
    CHECK(block->write_ptr_.load(std::memory_order_relaxed) == block->buf_);
    CHECK(block->read_pos() == 0 && block->read_size() == 0 && block->is_alloc_finish());
    CHECK(!block->reset(loan.generation() + 1)); // Do not reset a generation still in use.
    auto before = block->read_snapshot();
    CHECK(before.block_id == reinterpret_cast<std::uintptr_t>(block));
    CHECK(before.sequence == loan.generation() && before.read_size == 0 && before.published_size == 0);

    // Simulate Writer's batched local cursor: copying alone does not publish; the store does.
    std::memcpy(block->buf_, "12345678", 8);
    CHECK(block->readable().empty());
    block->write_ptr_.store(block->buf_ + 4, std::memory_order_release);
    CHECK(block->write_pos() == 4 && !block->is_alloc_finish());
    CHECK(block->writable().data() == block->buf_ + 4 && block->writable().size() == 4);
    CHECK(block->commit_write(2)); // The wrapper must also resume from Writer's published end.
    CHECK(block->write_ptr_.load(std::memory_order_acquire) == block->buf_ + 6);
    CHECK(block->consume(3));
    Block::ReadSnapshot snapshot;
    auto bytes = block->readable(snapshot);
    CHECK(snapshot.block_id == before.block_id && snapshot.sequence == before.sequence);
    CHECK(snapshot.read_size == 3 && snapshot.published_size == 6);
    CHECK(bytes.size() == 3 && std::memcmp(bytes.data(), "456", 3) == 0);
    CHECK(block->read_pos() == 3 && block->read_size() == 3);

    block->write_ptr_.store(block->buf_ + 8, std::memory_order_release); // Flush the final tail.
    CHECK(block->to_write_finished() && !block->finish());
    CHECK(block->to_leave_writer() && !block->finish());
    CHECK(block->consume(5) && block->is_alloc_finish() && block->finish());
    auto reused = pool.get();
    CHECK(reused.get() == block && !loan.is_valid());
    auto reset = reused->read_snapshot();
    CHECK(reset.block_id == before.block_id && reset.sequence != before.sequence);
    CHECK(reset.sequence == reused.generation() && reset.read_size == 0 && reset.published_size == 0);
    CHECK(reused->write_ptr_.load(std::memory_order_relaxed) == reused->buf_);
    CHECK(reused->to_write_finished() && reused->to_leave_writer() && reused->finish());
    CHECK(pool.all_released());
}

void streaming() {
    Pool<SpscWriteBlock> pool(1, 1, SpscBlockFactory<SpscWriteBlock>{8});
    auto loan = pool.get(); auto* block = loan.get();
    CHECK(block->buf_ == block->data());
    CHECK(block->readable().empty() && !block->finish() && !block->to_leave_writer());
    std::memcpy(block->writable().data(), "12345678", 8);
    CHECK(!block->commit_write(9) && block->readable().empty());
    CHECK(block->commit_write(3));
    CHECK(block->readable().size() == 3 && std::memcmp(block->readable().data(), "123", 3) == 0);
    CHECK(!block->consume(4) && block->readable().size() == 3);
    CHECK(block->consume(2));
    CHECK(block->writable().size() == 5); // Consumption does not turn this into a circular buffer.
    CHECK(block->commit_write(5) && block->writable().empty());
    CHECK(block->readable().size() == 6);
    CHECK(std::memcmp(block->readable().data(), "345678", 6) == 0);
    CHECK(block->to_write_finished());
    CHECK(!block->commit_write(0) && !block->to_write_finished());
    CHECK(!block->finish()); // The writer has not relinquished access; early return is forbidden.
    CHECK(block->to_leave_writer() && !block->to_leave_writer());
    CHECK(!block->finish()); // An unread tail remains.
    CHECK(block->consume(6) && block->finish());
    CHECK(!loan.is_valid() && !loan.recycle());
    auto empty = pool.get();
    CHECK(empty->readable().empty() && empty->writable().size() == 8);
    CHECK(empty->to_write_finished() && empty->to_leave_writer() && empty->finish());
}

void handoff() {
    Pool<SpscReadBlock> pool(1, 1, SpscBlockFactory<SpscReadBlock>{8});
    auto loan = pool.get(); auto* block = loan.get();
    CHECK(!block->is_ready() && block->readable().empty() && !block->finish());
    std::memcpy(block->data(), "hello", 5);
    CHECK(!block->publish(9) && !block->is_ready());
    CHECK(block->publish(5) && block->is_ready());
    CHECK(block->readable().size() == 5 && std::memcmp(block->readable().data(), "hello", 5) == 0);
    CHECK(!block->publish(3) && block->readable().size() == 5);
    CHECK(block->finish() && !pool.is_valid(loan));
    auto empty = pool.get();
    CHECK(!empty->is_ready());
    CHECK(empty->publish(0) && empty->is_ready() && empty->readable().empty());
    CHECK(empty->finish() && pool.all_released());
}

struct CountedReadBlock : SpscReadBlock {
    using SpscReadBlock::SpscReadBlock;
    inline static unsigned destroyed = 0;
    ~CountedReadBlock() noexcept { ++destroyed; }
};

void reuse() {
    CountedReadBlock::destroyed = 0;
    {
        Pool<CountedReadBlock> pool(0, 1, SpscBlockFactory<CountedReadBlock>{64}, 0);
        Pool<CountedReadBlock>::Loan old;
        for (unsigned i = 0; i < 100; ++i) {
            auto loan = pool.get();
            CHECK(loan && loan.generation() != old.generation());
            CHECK(!old.is_valid() && !old.recycle());
            CHECK(loan->publish(0) && loan->finish());
            CHECK(pool.resource_count() == 0 && pool.will_dispose_count() == 1);
            CHECK(CountedReadBlock::destroyed == i);
            old = loan;
        }
    }
    CHECK(CountedReadBlock::destroyed == 100);
}

std::byte pattern(std::size_t round, std::size_t pos) {
    return static_cast<std::byte>((round * 13 + pos * 7) & 255);
}

void concurrent_stream() {
    constexpr std::size_t rounds = 2000, capacity = 127;
    for (std::size_t cache : {0u, 2u}) {
        Pool<SpscWriteBlock> pool(0, 4, SpscBlockFactory<SpscWriteBlock>{capacity}, cache);
        SpscQueue<Pool<SpscWriteBlock>::Loan> active(2), completed(2);
        std::thread reader([&] {
            for (std::size_t round = 0; round < rounds; ++round) {
                while (!active.front()) std::this_thread::yield();
                auto loan = *active.front(); active.pop();
                auto* block = loan.get();
                std::size_t consumed = 0;
                // Read a published prefix while the writer continues, but never return early even if that prefix is consumed.
                while (consumed != capacity) {
                    auto bytes = block->readable();
                    for (std::size_t i = 0; i < bytes.size(); ++i) CHECK(bytes[i] == pattern(round, consumed + i));
                    consumed += bytes.size();
                    CHECK(block->consume(bytes.size()));
                    CHECK(!block->finish());
                    if (bytes.empty()) std::this_thread::yield();
                }
                while (!completed.front()) std::this_thread::yield();
                auto done = *completed.front(); completed.pop();
                CHECK(done.get() == loan.get() && done.generation() == loan.generation());
                CHECK(block->to_leave_writer() && block->finish());
                CHECK(!loan.is_valid() && !loan.recycle());
            }
        });
        for (std::size_t round = 0; round < rounds; ++round) {
            auto loan = pool.wait_get(std::chrono::seconds{10}); CHECK(loan);
            while (!active.try_push(loan)) std::this_thread::yield();
            std::size_t written = 0;
            while (written != capacity) {
                const auto count = (std::min)(std::size_t{11}, capacity - written);
                auto bytes = loan->writable();
                for (std::size_t i = 0; i < count; ++i) bytes[i] = pattern(round, written + i);
                if (round % 2 == 0) // The direct-publication path actually used by Writer.
                    loan->write_ptr_.store(bytes.data() + count, std::memory_order_release);
                else
                    CHECK(loan->commit_write(count));
                written += count;
            }
            CHECK(loan->to_write_finished());
            while (!completed.try_push(loan)) std::this_thread::yield();
            // Do not dereference the old Loan after handoff.
        }
        reader.join();
        CHECK(pool.all_released() && active.empty() && completed.empty());
    }
}

void concurrent_handoff() {
    constexpr std::size_t rounds = 4000, capacity = 97;
    Pool<SpscReadBlock> pool(0, 4, SpscBlockFactory<SpscReadBlock>{capacity}, 1);
    SpscQueue<Pool<SpscReadBlock>::Loan> queue(2);
    std::thread reader([&] {
        for (std::size_t round = 0; round < rounds; ++round) {
            while (!queue.front()) std::this_thread::yield();
            auto loan = *queue.front(); queue.pop();
            while (!loan->is_ready()) std::this_thread::yield();
            auto bytes = loan->readable();
            CHECK(bytes.size() == round % (capacity + 1));
            for (std::size_t i = 0; i < bytes.size(); ++i) CHECK(bytes[i] == pattern(round, i));
            CHECK(loan->finish());
        }
    });
    for (std::size_t round = 0; round < rounds; ++round) {
        auto loan = pool.wait_get(std::chrono::seconds{10}); CHECK(loan);
        while (!queue.try_push(loan)) std::this_thread::yield();
        const auto size = round % (capacity + 1);
        for (std::size_t i = 0; i < size; ++i) loan->data()[i] = pattern(round, i);
        CHECK(loan->publish(size)); // Stop accessing the resource after release-publication.
    }
    reader.join();
    CHECK(pool.all_released() && queue.empty());
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string_view name(argv[1]);
    if (name == "storage") storage();
    else if (name == "writer_fields") writer_fields();
    else if (name == "streaming") streaming();
    else if (name == "handoff") handoff();
    else if (name == "reuse") reuse();
    else if (name == "concurrent_stream") concurrent_stream();
    else if (name == "concurrent_handoff") concurrent_handoff();
    else return 2;
    std::cout << "PASS " << name << '\n';
}
