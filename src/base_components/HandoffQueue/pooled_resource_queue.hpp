#pragma once

#include "claim_queue.hpp"
#include "TwoBufQueue/src/base_components/WarmPool/warm_pool.hpp"

namespace TwoBufQueue {

// 可独立使用的池化资源队列，没有回调或线程；Queue 参数允许替换槽位/领取实现。
template<class Pool, class Queue = ClaimQueue<typename Pool::ResourceType>>
class PooledResourceQueue {
public:
    using Resource = typename Pool::ResourceType;
    using Loan = typename Pool::Loan;
    template<bool C> using Ticket = typename Queue::template Ticket<C>;
    using ProducerLoan = typename Queue::ProducerLoan;
    using Claim = typename Queue::Claim;
    static_assert(PoolResource<Resource>);
    static_assert(Pool::concurrent, "PooledResourceQueue requires a concurrent WarmPool");
    struct AcquireResult {
        AcquireStatus status;
        ProducerLoan loan;
        explicit operator bool() const noexcept { return status == AcquireStatus::Success; }
    };

    PooledResourceQueue(Pool& pool, std::size_t capacity, bool enable_stats = false,
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
        // 凭证保留原来的池身份和代号，仅切换完成入口；Loan 本身不增加字段。
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
    template<bool C> [[nodiscard]] HandoffState state(const Ticket<C>& ticket) const noexcept {
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
    template<class T> bool finish_ticket(T& ticket) noexcept {
        auto completed = queue_.begin_finish(ticket);
        if (!completed) return false;
        const auto loan = completed.loan();
        if (!pool_.recycle(loan)) std::terminate();
        if (!queue_.complete_finish(completed)) std::terminate();
        return true;
    }
    // 只构造节点凭证供池校验，不需要已经可能失效的资源指针。
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
        // 池在稳定 Node 上按代号串行判定，只有归还成功者更新 received 计数。
        // 不访问资源内容，也不依赖已经释放/复用的队列槽位。
        if (!self.pool_.recycle(identity(owner, node, generation))) return false;
        self.queue_.complete_received();
        return true;
    }
    Pool& pool_;
    Queue queue_;
    const LoanOperations received_operations_;
};

template<class Pool>
PooledResourceQueue(Pool&, std::size_t, bool = false, CancelClaimMode = CancelClaimMode::QueueTail)
    -> PooledResourceQueue<Pool>;

} // namespace TwoBufQueue
