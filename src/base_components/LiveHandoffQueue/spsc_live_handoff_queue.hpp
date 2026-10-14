// SpscLiveHandoffQueue combines a resource pool with an SPSC producer-consumer queue.
// Producers borrow and populate resources; completed consumption returns resources to the pool.
// Producer: borrow from the pool (try_acquire) -> write/publish Message -> hand off (send / send_and_acquire).
// Consumer: claim each completed record with try_claim -> finish on success -> continue to the next record.
//                                                    -> otherwise cancel_claim and retain the record at the head for retry.
// Differences from a conventional SPSC queue:
// 1. Production needs a resource pool, which may be replaced by a similar pool or an upstream completion queue.
// 2. After earlier completed records, the consumer may read the resource still being produced. Each {finished, next} record keeps current-resource reads aligned with the completion queue.
// 3. claim/cancel replaces pop.
// Compared with pop, claim/cancel: a. simplifies consumers by keeping pending resources in the queue;
//                               b. avoids losing a removed resource when a consumer exits unexpectedly;

#pragma once

#include "TwoBufQueue/src/base_components/WarmPool/warm_pool.hpp"
#include "TwoBufQueue/src/utils/spsc_queue.hpp"
#include <cassert>

namespace TwoBufQueue {

enum class LiveAcquireStatus { Success, ProducerBusy, QueueFull, PoolLimit, ResourceError, Closed };
struct LiveHandoffStats {
    std::uint64_t acquired = 0, queue_full = 0, pool_limit = 0, resource_error = 0;
};

// Ordered SPSC handoff. Resource publication cursors and read logic do not belong to this queue.
// Except is_closed/pending_count/stats, each interface is used only by its owning side; stop both sides before destruction.
template<class TPool>
class SpscLiveHandoffQueue {
public:
    using Pool = TPool;
    using Resource = typename TPool::ResourceType;
    using Loan = typename TPool::Loan;
    static_assert(TPool::concurrent, "SpscLiveHandoffQueue requires a concurrent pool");
    template<bool TConsumer> class Ticket {
        friend class SpscLiveHandoffQueue;
    public:
        Ticket() noexcept = default;
        // A consumer ticket represents a dequeued record; begin records are valid too, with empty get().
        explicit operator bool() const noexcept {
            if constexpr (TConsumer) return owner_ != nullptr;
            else return bool(loan_);
        }
        Resource* get() const noexcept { return loan_.get(); }
        Resource* operator->() const noexcept { return get(); }
        Resource& operator*() const noexcept { return *get(); }
    private:
        const SpscLiveHandoffQueue* owner_ = nullptr;
        std::uint64_t id_ = 0;
        Loan loan_;
    };
    using ProducerLoan = Ticket<false>;
    using Claim = Ticket<true>;
    class Record : public Ticket<true> {
        friend class SpscLiveHandoffQueue;
    public:
        // Borrow the next producer resource only; read published data after earlier records finish, and never return it yourself.
        [[nodiscard]] Loan next() const noexcept { return next_; }
    private:
        Loan next_;
    };
    struct AcquireResult {
        LiveAcquireStatus status;
        ProducerLoan loan;
        explicit operator bool() const noexcept { return status == LiveAcquireStatus::Success; }
    };
    struct Callbacks {
        void (*on_send)(Resource&) noexcept = nullptr;
        // Preconditions for finish: retain an unfinished Record and resume it on the next attempt.
        bool (*can_finish)(const Resource&) noexcept = nullptr;
        // An optional callback performs the final return; otherwise the queue returns the resource directly to the external pool.
        // The callback may immediately destroy the resource; the queue does not access it afterward.
        void (*on_finish)(Resource&) noexcept = nullptr;
    };
    using NotifyCallback = void (*)(void*) noexcept;

    SpscLiveHandoffQueue(TPool& pool, std::size_t capacity, Callbacks callbacks = {}, bool enable_stats = false)
        : pool_(pool), entries_(checked_capacity(capacity)), callbacks_(callbacks), enable_stats_(enable_stats) {}
    SpscLiveHandoffQueue(const SpscLiveHandoffQueue&) = delete;
    SpscLiveHandoffQueue& operator=(const SpscLiveHandoffQueue&) = delete;
    ~SpscLiveHandoffQueue() noexcept {
        if (producer_ || claim_ || popped_id_ != finished_id_ || current_ || !entries_.empty()) {
            std::fputs("destroying SpscLiveHandoffQueue before draining handoffs\n", stderr);
            std::terminate();
        }
    }

    [[nodiscard]] TPool& pool() const noexcept { return pool_; }
    [[nodiscard]] const Callbacks& callbacks() const noexcept { return callbacks_; }

#pragma region Producer side (writer thread)
public:
    // Producer: duplicate acquisition is an API error; never implicitly obtain another block or return the existing one.
    [[nodiscard]] AcquireResult try_acquire() noexcept {
        if (producer_) return {LiveAcquireStatus::ProducerBusy, {}};
        auto result = acquire();
        if (result) enqueue({}, producer_.loan_); // Begin record: exposes the new current block to the consumer.
        return result;
    }
    [[nodiscard]] ProducerLoan current_producer() const noexcept { return producer_; }
    bool send() noexcept {
        if (!producer_) return false;
        const auto old = producer_.loan_;
        if (callbacks_.on_send) callbacks_.on_send(*old);
        producer_ = {};
        enqueue(old, {});
        notify_read_available();
        return true;
    }
    // Like the original get_next_mem, a successful switch publishes exactly one {old block, new block} record.
    // Failure still hands off the old block; the caller may subsequently wait/retry try_acquire.
    [[nodiscard]] AcquireResult send_and_acquire() noexcept {
        const auto finished = producer_;
        auto finished_loan = finished ? finished.loan_ : Loan{};
        if (finished) {
            if (callbacks_.on_send) callbacks_.on_send(*finished);
            producer_ = {};
        }
        auto result = acquire();
        auto next_loan = producer_ ? producer_.loan_ : Loan{};
        if (finished || producer_) {
            enqueue(finished_loan, next_loan); // {old block, new block}; acquisition failure yields {old block, empty}.
            notify_read_available();
        }
        return result;
    }
    // Reject new acquisitions without implicitly sending current; an existing producer ticket may still be sent.
    void close() noexcept { closed_.store(true, std::memory_order_release); }
    // External wakeup only; do not reenter the producer interface.
    [[nodiscard]] bool has_read_available_callback() const noexcept { return notify_ != nullptr; }

private:
    // Borrow from the pool and set producer_ without enqueueing; try_acquire / send_and_acquire publish the handoff record.
    AcquireResult acquire() noexcept {
        auto status = LiveAcquireStatus::Closed;
        Loan next;
        if (!is_closed()) {
            // Reserve this record's slot plus one extra so the current producer block can always be handed off.
            if (entries_.capacity() - entries_.size() < 2) {
                status = LiveAcquireStatus::QueueFull;
                if (enable_stats_) queue_full_.fetch_add(1, std::memory_order_relaxed);
            } else {
                PoolGetStatus pool_status;
                next = pool_.get(pool_status);
                if (next) {
                    status = LiveAcquireStatus::Success;
                    producer_ = make_ticket<false>(next, next_id(producer_id_));
                    if (enable_stats_) acquired_.fetch_add(1, std::memory_order_relaxed);
                } else if (pool_status == PoolGetStatus::Limit) {
                    status = LiveAcquireStatus::PoolLimit;
                    if (enable_stats_) pool_limit_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    status = LiveAcquireStatus::ResourceError;
                    if (enable_stats_) resource_error_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        return {status, producer_};
    }
    void enqueue(Loan finished, Loan next) noexcept {
        if (!entries_.try_push(Entry{finished, next})) std::terminate(); // Reserved-capacity invariant.
    }

#pragma endregion

#pragma region Consumer side (reader thread)
public:
    // Claim the head without removing it; its queue slot stays occupied. Only one active Claim is allowed.
    [[nodiscard]] Claim try_claim() noexcept {
        assert(!claim_ && popped_id_ == finished_id_);
        if (claim_ || popped_id_ != finished_id_) return {};
        const auto* entry = entries_.front();
        if (!entry) return {};
        if (claim_id_ == std::numeric_limits<std::uint64_t>::max()) std::terminate();
        claim_ = make_ticket<true>(entry->finished, ++claim_id_);
        return claim_;
    }

    // Cancel without dequeueing or returning the resource; the next try_claim yields the same head record.
    void cancel_claim(Claim& claim) noexcept {
        const bool valid = matches(claim, claim_);
        assert(valid);
        if (!valid) return;
        claim_ = {};
        claim = {};
    }

    // Finish a Claim: dequeue, advance current and return any completed resource to the pool.
    void finish(Claim& claim) noexcept {
        const bool valid = matches(claim, claim_) && popped_id_ == finished_id_;
        assert(valid);
        if (!valid) return;
        const auto* entry = entries_.front();
        assert(entry);
        if (!entry) return;
        const auto loan = claim_.loan_;
        const auto next = entry->next;
        if (loan && callbacks_.can_finish && !callbacks_.can_finish(*loan)) {
            assert(false && "finish requires a fully processed resource");
            std::terminate();
        }
        claim_ = {};
        claim = {};
        entries_.pop();
        current_ = next;
        popped_id_ = next_id(popped_id_);
        finished_id_ = popped_id_;
        recycle_finished(loan);
    }

    // Remove one record and immediately release its queue slot, without returning its resource.
    // {empty, next} is a valid Record with empty get(); it still requires finish acknowledgement.
    // Consecutive pop calls are allowed. The caller retains pending records and handles/finishes them in dequeue order.
    [[nodiscard]] Record try_pop() noexcept {
        assert(!claim_);
        if (claim_) return {};
        const auto* entry = entries_.front();
        if (!entry) return {};
        Record record;
        static_cast<Ticket<true>&>(record) = make_ticket<true>(entry->finished, next_id(popped_id_));
        record.next_ = entry->next;
        current_ = entry->next;
        entries_.pop();
        return record;
    }

    // Consumer-side current is the last popped next; query only, without dequeueing or reading producer_.
    // When records remain queued, current may lag behind the tail's next; snapshot_for_read fixes this round's boundary first.
    // Finish all previously popped pending records before reading the resource; never return current directly.
    [[nodiscard]] Loan current() const noexcept { return current_; }

    // Fix this round's queued record count: use the last record's next, or current() if the queue is empty.
    // Excludes already popped pending records; process those and this round's records before reading the returned block.
    [[nodiscard]] Loan snapshot_for_read(std::size_t& count) const noexcept {
        count = entries_.size();
        if (!count) return current();
#ifndef NDEBUG
        // Compare tickets only; do not dereference resources or access the producer's producer_.
        const auto& head = entries_.front()->finished;
        assert(head.get() == current_.get() && head.generation() == current_.generation());
#endif
        return entries_.front(count - 1)->next;
    }

    // Consumer-side query for transferring pending work to another stopped reader; it does not extend resource lifetime.
    [[nodiscard]] bool is_pending(const Record& record) const noexcept {
        return record.owner_ == this && record.id_ > finished_id_ && record.id_ <= popped_id_;
    }
    // Acknowledge a dequeued record and return its resource without touching queue slots. Call finish in dequeue order.
    void finish(Record& record) noexcept {
        const bool valid = is_pending(record) && record.id_ - finished_id_ == 1;
        assert(valid);
        if (!valid) return;
        const auto loan = record.loan_;
        if (loan && callbacks_.can_finish && !callbacks_.can_finish(*loan)) {
            assert(false && "finish requires a fully processed resource");
            std::terminate();
        }
        finished_id_ = record.id_;
        record = {};
        if (!loan) return; // Begin records have no old resource; invoke neither the resource callback nor recycle.
        recycle_finished(loan);
    }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] bool is_drained() const noexcept {
        return is_closed() && popped_id_ == finished_id_ && !current_ && entries_.empty();
    }

#pragma endregion

#pragma region Queries / cross-side operations
public:
    [[nodiscard]] bool is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    [[nodiscard]] std::size_t pending_count() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return entries_.capacity(); }
    [[nodiscard]] bool owns(const Loan& loan) const noexcept { return pool_.owns(loan); }
    [[nodiscard]] LiveHandoffStats stats() const noexcept {
        return {acquired_.load(std::memory_order_relaxed), queue_full_.load(std::memory_order_relaxed),
                pool_limit_.load(std::memory_order_relaxed), resource_error_.load(std::memory_order_relaxed)};
    }
    // Install/remove while the producer is quiescent; external wakeup only, never reenter production.
    void set_read_available_callback(NotifyCallback callback, void* context) noexcept {
        notify_ = callback;
        notify_context_ = context;
    }
    // Notify the consumer that new published data may be read; the producer calls this after send, send_and_acquire or flush.
    void notify_read_available() noexcept {
        if (notify_) notify_(notify_context_);
    }

#pragma endregion

private:
    void recycle_finished(const Loan& loan) noexcept {
        if (!loan) return;
        if (callbacks_.on_finish) callbacks_.on_finish(*loan);
        else if (!pool_.recycle(loan)) std::terminate();
    }
    struct Entry { Loan finished, next; };
    static std::size_t checked_capacity(std::size_t capacity) {
        if (capacity < 2) throw std::invalid_argument("SpscLiveHandoffQueue capacity must be at least 2");
        return capacity;
    }
    static std::uint64_t next_id(std::uint64_t& id) noexcept {
        if (id == std::numeric_limits<std::uint64_t>::max()) std::terminate();
        return ++id;
    }
    template<bool C> Ticket<C> make_ticket(Loan loan, std::uint64_t id) const noexcept {
        Ticket<C> ticket;
        ticket.owner_ = this;
        ticket.id_ = id;
        ticket.loan_ = loan;
        return ticket;
    }
    template<bool C> bool matches(const Ticket<C>& input, const Ticket<C>& active) const noexcept {
        return active && input.owner_ == this && input.id_ == active.id_;
    }
    // Data members: preserve cache-line separation.
    TPool& pool_;
    SpscQueue<Entry> entries_;
    const Callbacks callbacks_;
    const bool enable_stats_;
    NotifyCallback notify_ = nullptr;
    void* notify_context_ = nullptr;
    std::atomic<bool> closed_{false};
    alignas(64) ProducerLoan producer_;
    std::uint64_t producer_id_ = 0;
    alignas(64) Claim claim_;
    std::uint64_t claim_id_ = 0;
    alignas(64) Loan current_;
    std::uint64_t popped_id_ = 0, finished_id_ = 0;
    alignas(64) std::atomic<std::uint64_t> acquired_{0}, queue_full_{0}, pool_limit_{0}, resource_error_{0};
};

// Convenience resource-type specialization: owns a default WarmPool internally.
// Passing WarmPool<TResource> still uses the primary template and supports an externally shared pool.
template<PoolResource TResource>
class SpscLiveHandoffQueue<TResource>
    : private WarmPool<TResource>, public SpscLiveHandoffQueue<WarmPool<TResource>> {
    using OwnedPool = WarmPool<TResource>;
    using QueueBase = SpscLiveHandoffQueue<OwnedPool>;
public:
    using Callbacks = typename QueueBase::Callbacks;

    SpscLiveHandoffQueue(std::size_t min_count, std::size_t max_count, std::size_t capacity,
                        Callbacks callbacks = {}, bool enable_stats = false)
        : OwnedPool(min_count, max_count),
          QueueBase(static_cast<OwnedPool&>(*this), capacity, callbacks, enable_stats) {}

    [[nodiscard]] bool all_released() const noexcept { return OwnedPool::all_released(); }
};

} // namespace TwoBufQueue
