// HandoffQueue combines a resource pool with a producer-consumer queue, supporting SPSC, MPSC, SPMC, MPMC and Sync.
// Producers borrow pooled resources and populate them; completed consumption returns each resource to the pool.
// Producer: borrow from the pool (try_acquire) -> populate the resource -> hand it to the queue (send).
//                                          -> cannot produce now -> return the resource and reserved slot (discard).
// Consumer: claim a queued resource (try_claim) -> callback returns true -> acknowledge and return it (finish).
//                                              -> callback returns false -> cancel; cancel_claim_mode controls retry order.
// Differences from a conventional producer-consumer queue:
// 1. An external pool owns reusable resources; this example uses WarmPool. An upstream queue needs a separate adapter.
// 2. Claiming is not completion: cancel when processing is not possible; the same or another consumer can retry.
//    Keep prioritizes earlier enqueued claims; QueueTail returns them to the tail. Both retain the resource and slot and do not undo business effects.
// 3. try_acquire also reserves a queue slot, so a valid send cannot fail because the queue is full. Configure pool and queue capacities separately.
//    The producer exclusively owns an unsent resource and must stop accessing it after send; consumers handle handed-off resources only.
// 4. The built-in Reader claims, invokes callbacks and finishes/cancels. Applications supply bool(Resource&) noexcept.
// SPSC/MPSC use one built-in reader; SPMC/MPMC allow multiple readers. Choose continuous polling or signal-based waits.
//
// Compared with pop, claim/cancel: a. simplifies consumers by keeping pending resources in the queue;
//                               b. avoids losing a removed resource when a consumer exits unexpectedly;
//                               c. reorders cancelled claims in the queue so earlier eligible work can be claimed first.
#pragma once

#include "pooled_resource_queue.hpp"
#include "handoff_reader.hpp"

namespace TwoBufQueue {

// Scheduling facade: compose the resource queue and consumer behind one producer/consumer interface.
// Owns no slots, queue locks, resource state or threads; the separate components perform those operations.
template<class Pool, class Callback, class ResourceQueue = PooledResourceQueue<Pool>>
class HandoffQueue {
public:
    using Resource = typename ResourceQueue::Resource;
    using Loan = typename ResourceQueue::Loan;
    template<bool C> using Ticket = typename ResourceQueue::template Ticket<C>;
    using ProducerLoan = typename ResourceQueue::ProducerLoan;
    using Claim = typename ResourceQueue::Claim;
    using AcquireResult = typename ResourceQueue::AcquireResult;
    using NotifyCallback = void (*)(void*) noexcept;

    HandoffQueue(Pool& pool, HandoffOptions options, Callback callback)
        : options_(validate_handoff_options(options)),
          queue_(pool, options_.queue_capacity, options_.enable_stats, options_.cancel_claim_mode),
          reader_(queue_, options_, std::move(callback)) {}
    HandoffQueue(const HandoffQueue&) = delete;
    HandoffQueue& operator=(const HandoffQueue&) = delete;
    ~HandoffQueue() noexcept { close(); stop(); }

    [[nodiscard]] AcquireResult try_acquire() noexcept { return queue_.try_acquire(); }
    bool send(ProducerLoan& loan) noexcept {
        if (!queue_.send(loan)) return false;
        notify_read_available();
        reader_.notify();
        return true;
    }
    bool discard(ProducerLoan& loan) noexcept { return queue_.discard(loan); }
    [[nodiscard]] Loan pop() noexcept { return queue_.pop(); }
    [[nodiscard]] Loan try_receive() noexcept { return queue_.try_receive(); }
    [[nodiscard]] Claim try_claim() noexcept { return queue_.try_claim(); }
    bool finish(Loan& loan) noexcept { return queue_.finish(loan); }
    bool finish(Claim& claim) noexcept { return queue_.finish(claim); }
    template<class TicketType> [[nodiscard]] HandoffState state(const TicketType& ticket) const noexcept {
        return queue_.state(ticket);
    }
    bool cancel_claim(Claim& claim) noexcept {
        if (!queue_.cancel_claim(claim)) return false;
        notify_read_available();
        if (options_.mode != HandoffMode::Sync) reader_.notify();
        return true;
    }
    // Producers must be quiescent while installing/removing this callback; notify only, never reenter the production interface.
    void set_read_available_callback(NotifyCallback callback, void* context) noexcept {
        notify_ = callback;
        notify_context_ = context;
    }
    void retry_pending() noexcept { reader_.process_pending(); }
    void close() noexcept { queue_.close(); }
    [[nodiscard]] bool is_closed() const noexcept { return queue_.is_closed(); }
    [[nodiscard]] bool is_drained() const noexcept {
        return queue_.is_drained() && !reader_.is_processing();
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return queue_.capacity(); }
    [[nodiscard]] HandoffStats stats() const noexcept { return queue_.stats(); }
    [[nodiscard]] HandoffMode mode() const noexcept { return options_.mode; }
    bool start() noexcept { return reader_.start(); }
    void stop() noexcept { reader_.stop(); }
    [[nodiscard]] bool is_running() const noexcept { return reader_.is_running(); }

private:
    void notify_read_available() noexcept {
        if (notify_) notify_(notify_context_);
    }

    const HandoffOptions options_;
    ResourceQueue queue_;
    HandoffReader<ResourceQueue, Callback> reader_;
    NotifyCallback notify_ = nullptr;
    void* notify_context_ = nullptr;
};

template<class Pool, class Callback>
HandoffQueue(Pool&, HandoffOptions, Callback) -> HandoffQueue<Pool, Callback>;

} // namespace TwoBufQueue
