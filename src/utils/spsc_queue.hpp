#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace TwoBufQueue {

// General SPSC FIFO independent of logging, WriterBlock<> and callbacks.
// Capacity is fixed at construction; the queue allocates nothing at runtime. TValue determines its own construction/destruction costs.
// Construct elements on enqueue and destroy them on pop; supports non-default-constructible and move-only types.
// Stop producer and consumer before destruction; front pointers are valid only until pop.
template<class TValue>
class SpscQueue {
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "SpscQueue requires lock-free atomic indices");
    static_assert(std::is_nothrow_destructible_v<TValue>,
                  "SpscQueue requires a non-throwing element destructor");

public:
    // capacity is the usable element count; one extra slot distinguishes full from empty.
    // Zero capacity is permitted: always empty and every enqueue returns false.
    explicit SpscQueue(std::size_t capacity)
        : slot_count_(checked_slot_count(capacity)),
          slots_(std::make_unique<Slot[]>(slot_count_)) {}

    ~SpscQueue() noexcept {
        // Both sides have stopped; destroy only elements still in the queue.
        auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_relaxed);
        while (read != write) {
            std::destroy_at(element(read));
            read = next_index(read);
        }
    }
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Producer only; full queues neither construct elements, move arguments nor change state.
    // If TValue's constructor throws, do not publish the write index, so the consumer never sees an unfinished object.
    template<class... TArgs>
    [[nodiscard]] bool try_emplace(TArgs&&... args)
        noexcept(std::is_nothrow_constructible_v<TValue, TArgs...>) {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = next_index(write);
        if (next == read_.load(std::memory_order_acquire)) return false;
        std::construct_at(reinterpret_cast<TValue*>(slots_[write].storage),
                          std::forward<TArgs>(args)...);
        write_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_push(const TValue& value)
        noexcept(std::is_nothrow_copy_constructible_v<TValue>) {
        return try_emplace(value);
    }
    [[nodiscard]] bool try_push(TValue&& value)
        noexcept(std::is_nothrow_move_constructible_v<TValue>) {
        return try_emplace(std::move(value));
    }

    // Consumer only; inspect without removing. A slot address remains non-null even when TValue itself is a null pointer.
    [[nodiscard]] const TValue* front() const noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return nullptr;
        return element(read);
    }
    [[nodiscard]] TValue* front() noexcept {
        return const_cast<TValue*>(std::as_const(*this).front());
    }

    // Consumer only: inspect the element offset positions beyond the head without dequeueing.
    // Collect a batch before processing it; the producer cannot overwrite these active slots until consumer pop.
    // Out-of-range returns nullptr; fix this round's count first instead of continually chasing producer enqueues.
    [[nodiscard]] const TValue* front(std::size_t offset) const noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        const auto count = write >= read ? write - read : slot_count_ - read + write;
        if (offset >= count) return nullptr;
        // Avoid read + offset before checking to prevent unsigned overflow with very large capacities.
        const auto tail = slot_count_ - read;
        return element(offset < tail ? read + offset : offset - tail);
    }

    // Consumer only, on a nonempty queue; call after business processing succeeds.
    // Destroy the old element before publishing the read index, then allow the producer to reuse its slot.
    void pop() noexcept {
        assert(!empty());
        const auto read = read_.load(std::memory_order_relaxed);
        std::destroy_at(element(read));
        read_.store(next_index(read), std::memory_order_release);
    }

    // Like front, empty is consumer-only; any thread may read size as an approximate snapshot.
    [[nodiscard]] bool empty() const noexcept { return front() == nullptr; }
    [[nodiscard]] std::size_t size() const noexcept {
        const auto read = read_.load(std::memory_order_acquire);
        const auto write = write_.load(std::memory_order_acquire);
        return write >= read ? write - read : slot_count_ - read + write;
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return slot_count_ - 1; }

private:
    // Reserve aligned storage without requiring default-constructible TValue; the queue explicitly manages active object lifetimes.
    struct Slot { alignas(TValue) std::byte storage[sizeof(TValue)]; };

    static std::size_t checked_slot_count(std::size_t capacity) {
        if (capacity == std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("SpscQueue capacity overflow");
        }
        return capacity + 1;
    }
    [[nodiscard]] std::size_t next_index(std::size_t index) const noexcept {
        return (index + 1) % slot_count_;
    }
    // Call only when the slot contains a fully constructed TValue.
    [[nodiscard]] TValue* element(std::size_t index) noexcept {
        return std::launder(reinterpret_cast<TValue*>(slots_[index].storage));
    }
    [[nodiscard]] const TValue* element(std::size_t index) const noexcept {
        return std::launder(reinterpret_cast<const TValue*>(slots_[index].storage));
    }

    const std::size_t slot_count_;
    std::unique_ptr<Slot[]> slots_;
    // Put each side's modified index on a separate cache line to avoid unnecessary mutual invalidation.
    alignas(64) std::atomic<std::size_t> read_{0};
    alignas(64) std::atomic<std::size_t> write_{0};
};

}  // namespace TwoBufQueue
