// HandoffQueue：结合内存池（资源池）的生产者消费者队列，支持 SPSC、MPSC、SPMC、MPMC 和 Sync 模式。
// 场景：生产者从资源池申请资源并生产产品，消费者处理完成后，组件将资源返还给资源池。
// 生产端：从【资源池】申请资源（try_acquire） -> 生产出产品   -> 交给【产品队列】（send）
//                                         -> 当前不能处理 -> 放弃生产，返还资源和预留槽位（discard）
// 消费端：从【产品队列】登记借出产品（try_claim） -> 回调返回 true  -> 确认完成，归还资源（finish）
//                                              -> 回调返回 false -> 取消本次领取，按 cancel_claim_mode 决定重试顺序
// 这里和一般生产者消费者队列的不同点：
//      1. 资源由外部资源池管理，可以反复借出和归还；本例使用 WarmPool，接入上一级队列需要另做适配。
//      2. 领取不等于完成：处理条件不足时可以取消领取，下次由当前消费者或其他消费者重新领取。
//         Keep 每次领取按照入队顺序排在前面的优先领取；QueueTail 放回队尾。均占用资源和槽位，不撤销业务操作。
//      3. try_acquire 同时预留队列槽位，合法的 send 不会因为队列满而失败；池容量和队列容量分别配置。
//         send 前资源由生产端独占，send 后生产端不再访问，消费者只处理已交接的产品。
//      4. 内置 Reader 负责领取、调用回调、确认或取消；用户只需提供 bool(Resource&) noexcept 回调。
// 执行模式：SPSC/MPSC 使用一个内置读取线程，SPMC/MPMC 可以配置多个；可选择持续轮询或等待信号。
//
// claim/cancel相比pop的优点是：a.消费者线程处理会更简单些，不需要消费者线程自己pending一个产品；
//                             b.当消费者线程异常结束的时候，不会产生资源泄漏
//                             c.cancle_claim可以回到queue中重新排序，这样获取的都是靠前的
#pragma once

#include "pooled_resource_queue.hpp"
#include "handoff_reader.hpp"

namespace TwoBufQueue {

// 调度入口：组合资源队列与消费器，保持统一的生产/消费接口。
// 不持有槽位、队列锁、资源状态或线程；具体操作由独立组件负责。
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
    // 安装/解除时生产者必须静止；回调只负责通知，不应重入队列生产接口。
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
