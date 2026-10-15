#pragma once

#include "claim_queue.hpp"
#include "TwoBufQueue/src/base_components/warm_pool/warm_pool.hpp"

namespace TwoBufQueue {

// Standalone pooled-resource queue without callbacks or threads; Queue permits alternate slot/claim implementations.
template<class TPool, class TQueue = ClaimQueue<typename TPool::ResourceType>>
class PooledResourceQueue {
public:
    using Resource = typename TPool::ResourceType;
    using Loan = typename TPool::Loan;
    template<bool TC> using Ticket = typename TQueue::template Ticket<TC>;
    using ProducerLoan = typename TQueue::ProducerLoan;
    using Claim = typename TQueue::Claim;
    static_assert(PoolResource<Resource>);
    static_assert(TPool::concurrent, "PooledResourceQueue requires a concurrent WarmPool");
    struct AcquireResult {
        AcquireStatus status;
        ProducerLoan loan;
        explicit operator bool() const noexcept { return status == AcquireStatus::Success; }
    };

    PooledResourceQueue(TPool& pool, std::size_t capacity, bool enable_stats = false,
                        CancelClaimMode cancel_claim_mode = CancelClaimMode::QueueTail)
        : pool_(pool), queue_(capacity, enable_stats, cancel_claim_mode),
          received_operations_{&validate_received, &recycle_received, this} {}
    PooledResourceQueue(const PooledResourceQueue&) = delete;
    PooledResourceQueue& operator=(const PooledResourceQueue&) = delete;
    ~PooledResourceQueue() noexcept {
        close();
        if (!is_drained()) {
            std::fputs("destroying PooledResourceQueue with outstanding resources\n", stderr);
            std::terminate();
        }
    }

    [[nodiscard]] AcquireResult try_acquire() noexcept {
        auto reserved = queue_.try_reserve();
        if (!reserved) return {reserved.status, {}};
        PoolGetStatus status;
        auto loan = pool_.get(status);
        if (!loan) {
            const auto reason = status == PoolGetStatus::Limit ? AcquireStatus::PoolLimit : AcquireStatus::ResourceError;
            if (!queue_.cancel_reservation(reserved.reservation, reason)) std::terminate();
            return {reason, {}};
        }
        auto producer = queue_.attach(reserved.reservation, loan);
        if (!producer) std::terminate();
        return {AcquireStatus::Success, producer};
    }
    bool send(ProducerLoan& loan) noexcept { return queue_.send(loan); }
    bool discard(ProducerLoan& loan) noexcept { return finish_ticket(loan); }
    [[nodiscard]] Loan try_receive() noexcept {
        auto loan = queue_.try_receive();
        // Preserve pool identity and generation in the ticket, replacing only its completion entry point; Loan gains no fields.
        if (loan) loan.operations_ = &received_operations_;
        return loan;
    }
    [[nodiscard]] Loan pop() noexcept { return try_receive(); }
    [[nodiscard]] Claim try_claim() noexcept { return queue_.try_claim(); }
    bool cancel_claim(Claim& claim) noexcept { return queue_.cancel_claim(claim); }
    bool finish(Claim& claim) noexcept { return finish_ticket(claim); }
    bool finish(Loan& loan) noexcept {
        if (loan.operations_ != &received_operations_ || !loan.finish()) return false;
        loan = {};
        return true;
    }
    template<bool TC> [[nodiscard]] HandoffState state(const Ticket<TC>& ticket) const noexcept {
        return queue_.state(ticket);
    }
    [[nodiscard]] HandoffState state(const Loan& loan) const noexcept {
        return loan.operations_ == &received_operations_ && loan.is_valid()
            ? HandoffState::Received : HandoffState::Invalid;
    }
    void close() noexcept { queue_.close(); }
    [[nodiscard]] bool is_closed() const noexcept { return queue_.is_closed(); }
    [[nodiscard]] bool is_drained() const noexcept { return queue_.is_drained(); }
    [[nodiscard]] bool has_ready() const noexcept { return queue_.has_ready(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return queue_.capacity(); }
    [[nodiscard]] HandoffStats stats() const noexcept { return queue_.stats(); }

private:
    template<class TValue> bool finish_ticket(TValue& ticket) noexcept {
        auto completed = queue_.begin_finish(ticket);
        if (!completed) return false;
        const auto loan = completed.loan();
        if (!pool_.recycle(loan)) std::terminate();
        if (!queue_.complete_finish(completed)) std::terminate();
        return true;
    }
    // Construct a node ticket for pool validation without requiring a possibly stale resource pointer.
    static Loan identity(void* owner, void* node, std::uint64_t generation) noexcept {
        return Loan(owner, node, nullptr, generation, nullptr, nullptr);
    }
    static bool validate_received(const LoanOperations& operations, void* owner, void* node,
                                  std::uint64_t generation) noexcept {
        auto& self = *static_cast<PooledResourceQueue*>(operations.context);
        return self.pool_.is_valid(identity(owner, node, generation));
    }
    static bool recycle_received(const LoanOperations& operations, void* owner, void* node,
                                 std::uint64_t generation) noexcept {
        auto& self = *static_cast<PooledResourceQueue*>(operations.context);
        // The pool validates generations serially on a stable Node; only the successful return updates received counts.
        // Do not access resource data or depend on an already released/reused queue slot.
        if (!self.pool_.recycle(identity(owner, node, generation))) return false;
        self.queue_.complete_received();
        return true;
    }
    TPool& pool_;
    TQueue queue_;
    const LoanOperations received_operations_;
};

template<class TPool>
PooledResourceQueue(TPool&, std::size_t, bool = false, CancelClaimMode = CancelClaimMode::QueueTail)
    -> PooledResourceQueue<TPool>;

} // namespace TwoBufQueue
