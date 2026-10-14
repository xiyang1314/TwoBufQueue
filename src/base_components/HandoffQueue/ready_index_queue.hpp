#pragma once

#include "handoff_types.hpp"
#include "lock_free_index_queue.hpp"
#include <algorithm>
#include <cassert>
#include <thread>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace TwoBufQueue {

// Ready-index scheduling: new work is FIFO; Keep cancellations are ordered separately by original dequeue order.
// Every index is in [0, capacity); requeue only after successful pop. Exactly one caller owns it at a time.
// QueueTail uses the original lock-free FIFO; Keep avoids scanning running tasks and remains O(1) without retries.
// Keep cancellation publication waits for short dequeues in progress, not business processing, so it is not guaranteed lock-free.
class ReadyIndexQueue {
    static constexpr std::uint64_t retry_bit = std::uint64_t{1} << 63;
    static constexpr std::uint64_t writer_bit = std::uint64_t{1} << 62;
    static constexpr std::uint64_t readers_mask = writer_bit - 1;
    struct Retry { std::uint64_t order; std::uint32_t index; };
    static bool later(const Retry& a, const Retry& b) noexcept { return a.order > b.order; }
public:
    explicit ReadyIndexQueue(std::size_t capacity, CancelClaimMode mode = CancelClaimMode::QueueTail)
        : mode_(validate_cancel_claim_mode(mode)), fifo_(capacity),
          orders_(mode == CancelClaimMode::Keep ? capacity : 0),
          retries_(mode == CancelClaimMode::Keep ? capacity : 0) {}
    ReadyIndexQueue(const ReadyIndexQueue&) = delete;
    ReadyIndexQueue& operator=(const ReadyIndexQueue&) = delete;

    bool try_push(std::uint32_t index) noexcept { return fifo_.try_push(index); }

    bool try_pop(std::uint32_t& index) noexcept {
        if (mode_ == CancelClaimMode::QueueTail) return fifo_.try_pop(index);
        auto phase = phase_.load(std::memory_order_acquire);
        for (;;) {
            // A cancellation/retry dequeue is modifying the indices; allow this try to fail temporarily rather than bypass old work.
            if (phase & writer_bit) return false;
            if (phase & retry_bit) {
                assert((phase & readers_mask) == 0);
                if (!phase_.compare_exchange_weak(phase, retry_bit | writer_bit,
                        std::memory_order_acquire, std::memory_order_acquire)) continue;
                assert(retry_count_ != 0);
                std::pop_heap(retries_.begin(), retries_.begin() + retry_count_, later);
                index = retries_[--retry_count_].index;
                phase_.store(retry_count_ ? retry_bit : 0, std::memory_order_release);
                return true;
            }
            if ((phase & readers_mask) == readers_mask) std::terminate();
            if (!phase_.compare_exchange_weak(phase, phase + 1,
                    std::memory_order_acquire, std::memory_order_acquire)) continue;
            std::uint64_t order;
            const bool found = fifo_.try_pop(index, &order);
            if (found) {
                assert(index < orders_.size());
                orders_[index] = order;
            }
            phase_.fetch_sub(1, std::memory_order_release);
            return found;
        }
    }

    bool requeue(std::uint32_t index) noexcept {
        if (mode_ == CancelClaimMode::QueueTail) return fifo_.try_push(index);
        auto phase = phase_.load(std::memory_order_acquire);
        for (;;) {
            if (!(phase & writer_bit) && phase_.compare_exchange_weak(phase,
                    phase | retry_bit | writer_bit, std::memory_order_acq_rel, std::memory_order_acquire)) break;
            pause_cpu();
            phase = phase_.load(std::memory_order_acquire);
        }
#ifdef BRQ_READY_QUEUE_TEST_HOOK
        BRQ_READY_QUEUE_TEST_HOOK(1); // New claims are blocked; wait for FIFO dequeues already in progress.
#endif
        while (phase_.load(std::memory_order_acquire) & readers_mask) pause_cpu();
        assert(index < orders_.size() && retry_count_ < retries_.size());
        retries_[retry_count_++] = {orders_[index], index};
        std::push_heap(retries_.begin(), retries_.begin() + retry_count_, later);
        phase_.store(retry_bit, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool has_ready() const noexcept {
        return (mode_ == CancelClaimMode::Keep &&
                (phase_.load(std::memory_order_acquire) & retry_bit)) || fifo_.has_ready();
    }

private:
    static void pause_cpu() noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
        _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }
    const CancelClaimMode mode_;
    LockFreeIndexQueue fifo_;
    // Allocated only for Keep; each in-flight slot has at most one retry entry. No runtime allocation.
    std::vector<std::uint64_t> orders_;
    std::vector<Retry> retries_;
    std::size_t retry_count_ = 0; // Accessed only by the writer_bit owner.
    alignas(64) std::atomic<std::uint64_t> phase_{0};
};

} // namespace TwoBufQueue
