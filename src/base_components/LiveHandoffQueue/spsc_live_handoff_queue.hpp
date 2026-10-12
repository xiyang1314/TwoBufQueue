// SpscLiveHandoffQueue： 结合内存池（资源池）的spsc生产者消费者队列
// 场景：生产者申请资源并生产产品，消费者处理完成后，组件将资源返还给资源池。
// 生产端：从【资源池】申请资源try_acquire -> 写入 Message 并发布 -> 交给【产品队列】（send / send_and_acquire）
// 消费端：每条完成记录通过 try_claim 领取 -> 成功则 finish -> 继续下一条
//                                       -> 没处理 则 cancel_claim 后仍留在队首，可再次领取。
// 这里和一般 SPSC 队列的不同点：
//      1. 生产者需要资源池生产；也用其他的类似池或队列，比如上一级的完成队列。
//      2. 消费者处理完前面的完成记录后，可读取当前正在生产的产品。异步读取正在生产的产品可能会和完成队列脱节，所以队列通过每条记录保存 {finished, next}的方式来保证不脱节
//      3. 这里添加了claim/cancel来替代pop
// claim/cancel相比pop的优点是：a.消费者线程处理会更简单些，不需要消费者线程自己pending一个产品；
//                             b.当消费者线程异常结束的时候，不会产生资源泄漏

#pragma once

#include "TwoBufQueue/src/base_components/WarmPool/warm_pool.hpp"
#include "TwoBufQueue/src/utils/spsc_queue.hpp"
#include <cassert>

namespace TwoBufQueue {

enum class LiveAcquireStatus { Success, ProducerBusy, QueueFull, PoolLimit, ResourceError, Closed };
struct LiveHandoffStats {
    std::uint64_t acquired = 0, queue_full = 0, pool_limit = 0, resource_error = 0;
};

// 单生产者、单消费者的顺序交接。资源的发布游标和读取逻辑不属于队列。
// 除 is_closed/pending_count/stats 外，接口分别只由所属端调用；析构前停止两端。
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
        // 消费凭证表示已出队的记录；开始记录也有效，但 get() 为空。
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
        // 仅借用下一生产资源；完成前面的记录后才可读其已发布内容，不得自行归还。
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
        // finish 的前置条件检查；尚未处理完时由调用方保留 Record，下次继续处理。
        bool (*can_finish)(const Resource&) noexcept = nullptr;
        // 指定时由回调负责最终归还；不指定则队列直接归还外部池。
        // 回调允许立即销毁资源，返回后队列不会再访问它。
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

#pragma region product端（写线程）
public:
    // 生产端：重复申请是调用错误，不能隐式取得下一块或返回已有块。
    [[nodiscard]] AcquireResult try_acquire() noexcept {
        if (producer_) return {LiveAcquireStatus::ProducerBusy, {}};
        auto result = acquire();
        if (result) enqueue({}, producer_.loan_); // 开始记录：读端据此看到新当前块
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
    // 与原 get_next_mem 一样，成功换块只发布一条 {旧块, 新块} 记录。
    // 获取失败也已经交出旧块；调用方随后可以等待/重试 try_acquire。
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
            enqueue(finished_loan, next_loan); // {旧块, 新块}；申请失败时 {旧块, 空}
            notify_read_available();
        }
        return result;
    }
    // 停止新申请，不隐式发送当前块。已有生产凭证仍可 send。
    void close() noexcept { closed_.store(true, std::memory_order_release); }
    // 仅作外部唤醒，不得重入生产接口。
    [[nodiscard]] bool has_read_available_callback() const noexcept { return notify_ != nullptr; }

private:
    // 只从池申请并设置 producer_，不入队；由 try_acquire / send_and_acquire 发布交接记录。
    AcquireResult acquire() noexcept {
        auto status = LiveAcquireStatus::Closed;
        Loan next;
        if (!is_closed()) {
            // 本次记录占一格，额外保留一格，保证当前生产块最终总能发送。
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
        if (!entries_.try_push(Entry{finished, next})) std::terminate(); // 预留容量不变量。
    }

#pragma endregion

#pragma region client端（读线程）
public:
    // 领取队首但不出队；队列槽位继续被占用。一次只能有一个活动 Claim。
    [[nodiscard]] Claim try_claim() noexcept {
        assert(!claim_ && popped_id_ == finished_id_);
        if (claim_ || popped_id_ != finished_id_) return {};
        const auto* entry = entries_.front();
        if (!entry) return {};
        if (claim_id_ == std::numeric_limits<std::uint64_t>::max()) std::terminate();
        claim_ = make_ticket<true>(entry->finished, ++claim_id_);
        return claim_;
    }

    // 取消领取但不出队、不归还资源；下次 try_claim 仍会得到同一条队首记录。
    void cancel_claim(Claim& claim) noexcept {
        const bool valid = matches(claim, claim_);
        assert(valid);
        if (!valid) return;
        claim_ = {};
        claim = {};
    }

    // 完成一条 Claim：出队并推进 current；有完成资源时归还池。
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

    // 每次直接取出一条记录，立即释放队列槽位，但不归还资源。
    // {空,next} 也返回有效 Record，get() 为空；同样需要 finish 确认。
    // 可以连续 pop；调用方保存未完成记录，并按出队顺序处理和 finish。
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

    // 消费端保存的 current（最后 pop 的 next）；只查询，不出队，不访问生产端 producer_。
    // 有队内记录时它可能落后于队尾 next；先用 snapshot_for_read 固定本轮边界。
    // 读取资源前必须处理完此前已 pop 的 pending，不得自行归还 current。
    [[nodiscard]] Loan current() const noexcept { return current_; }

    // 固定本轮队内记录数：有记录取最后一条 next，没有记录取 current()。
    // 不包含已 pop 的 pending；处理完 pending 和本轮记录后，才能读返回的块。
    [[nodiscard]] Loan snapshot_for_read(std::size_t& count) const noexcept {
        count = entries_.size();
        if (!count) return current();
#ifndef NDEBUG
        // 只比较凭证，不解引用资源，也不访问生产端 producer_。
        const auto& head = entries_.front()->finished;
        assert(head.get() == current_.get() && head.generation() == current_.generation());
#endif
        return entries_.front(count - 1)->next;
    }

    // 仅消费端查询，用于把 pending 交给另一个已停止的读端；不延长资源寿命。
    [[nodiscard]] bool is_pending(const Record& record) const noexcept {
        return record.owner_ == this && record.id_ > finished_id_ && record.id_ <= popped_id_;
    }
    // 仅确认已出队记录并归还资源，不再操作队列槽位。必须按出队顺序 finish。
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
        if (!loan) return; // 开始记录没有旧资源，不调用资源回调或 recycle。
        recycle_finished(loan);
    }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] bool is_drained() const noexcept {
        return is_closed() && popped_id_ == finished_id_ && !current_ && entries_.empty();
    }

#pragma endregion

#pragma region 查询 / 跨端
public:
    [[nodiscard]] bool is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    [[nodiscard]] std::size_t pending_count() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return entries_.capacity(); }
    [[nodiscard]] bool owns(const Loan& loan) const noexcept { return pool_.owns(loan); }
    [[nodiscard]] LiveHandoffStats stats() const noexcept {
        return {acquired_.load(std::memory_order_relaxed), queue_full_.load(std::memory_order_relaxed),
                pool_limit_.load(std::memory_order_relaxed), resource_error_.load(std::memory_order_relaxed)};
    }
    // 安装/解除时生产端静止；只作外部唤醒，不得重入生产接口。
    void set_read_available_callback(NotifyCallback callback, void* context) noexcept {
        notify_ = callback;
        notify_context_ = context;
    }
    // 通知消费者有新发布的数据可检查；由生产端在 send、send_and_acquire 或 flush 后调用。
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
    // 数据成员（保持缓存行分隔布局）
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

// 资源类型的便捷偏特化：队列内部拥有默认 WarmPool。
// 传 WarmPool<TResource> 仍使用上面的主模板，继续支持外部共享资源池。
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
