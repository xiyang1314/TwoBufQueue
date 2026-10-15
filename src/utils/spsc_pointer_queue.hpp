#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace TwoBufQueue {

// Fixed-capacity SPSC pointer ring queue.
// TValue is the pointee type; transfer non-null pointers without creating, destroying or owning pointees.
// Fix capacity at construction; no runtime allocation. Stop every access before destruction.
template<class TValue>
class SpscPointerQueue {
public:
    // capacity counts usable entries; one extra sentinel slot distinguishes full from empty.
    // Zero capacity is always empty and rejects every push.
    explicit SpscPointerQueue(std::size_t capacity)
        : slots_(slot_count(capacity), nullptr) {}

    SpscPointerQueue(const SpscPointerQueue&) = delete;
    SpscPointerQueue& operator=(const SpscPointerQueue&) = delete;

    // Producer only; value must be non-null. A full queue returns false without changing state.
    [[nodiscard]] bool try_push(TValue* value) noexcept {
        assert(value != nullptr);
        const std::size_t write = write_.load(std::memory_order_relaxed);
        const std::size_t next = (write + 1) % slots_.size();
        if (next == read_.load(std::memory_order_acquire)) return false;
        slots_[write] = value;
        write_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer only; inspect the head without removing it. Empty returns nullptr.
    [[nodiscard]] TValue* front() const noexcept {
        const std::size_t read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return nullptr;
        return slots_[read];
    }

    // Consumer only, on a nonempty queue; release the slot without destroying its pointee.
    // Process through front() first, then pop() after successful processing.
    void pop() noexcept {
        assert(!empty());
        const std::size_t read = read_.load(std::memory_order_relaxed);
        slots_[read] = nullptr;
        read_.store((read + 1) % slots_.size(), std::memory_order_release);
    }

    // Consumer only; uses the same empty check as front().
    [[nodiscard]] bool empty() const noexcept { return front() == nullptr; }

    // Any thread may query this approximate concurrent snapshot; it cannot replace try_push/front checks.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t read = read_.load(std::memory_order_acquire);
        const std::size_t write = write_.load(std::memory_order_acquire);
        return write >= read ? write - read : slots_.size() - read + write;
    }

private:
    static std::size_t slot_count(std::size_t capacity) {
        // Check before adding one to avoid overflow in the sentinel slot count.
        if (capacity == std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("SpscPointerQueue capacity overflow");
        }
        return capacity + 1;
    }

    std::vector<TValue*> slots_;
    // Keep reader/writer indices on separate cache lines, preserving the original queue layout.
    alignas(64) std::atomic<std::size_t> read_{0};
    alignas(64) std::atomic<std::size_t> write_{0};
};

}  // namespace TwoBufQueue
