#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <vector>

namespace TwoBufQueue {

namespace detail {
// 低位为 index + 1（0 表示空），其余位为不回绕的版本号。
class TaggedIndex {
public:
    explicit TaggedIndex(std::size_t count) : bits_(checked_bits(count)),
        mask_((std::uint64_t{1} << bits_) - 1), step_(mask_ + 1) {}
    std::uint32_t index(std::uint64_t value) const noexcept {
        return static_cast<std::uint32_t>(value & mask_);
    }
    std::uint64_t next(std::uint64_t old, std::uint32_t index) const noexcept {
        // 宁可 fail fast，也不能让暂停很久的线程遇到 ABA。
        if (old > std::numeric_limits<std::uint64_t>::max() - step_) std::terminate();
        return ((old + step_) & ~mask_) | index;
    }
private:
    static unsigned checked_bits(std::size_t count) {
        if (!count || count > std::numeric_limits<std::uint32_t>::max() - 1)
            throw std::invalid_argument("index capacity must be in [1, UINT32_MAX - 1]");
        return static_cast<unsigned>(std::bit_width(count));
    }
    unsigned bits_;
    std::uint64_t mask_, step_;
};
} // namespace detail

// 固定索引的 Treiber 空闲栈。索引只能在被 pop 后归还一次。
// next 也必须是原子的：落后的 pop 可能仍在读取已被其他线程复用的节点。
class LockFreeIndexStack {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
public:
    explicit LockFreeIndexStack(std::size_t capacity)
        : tag_(capacity), next_(capacity) {
        for (std::size_t i = 0; i < capacity; ++i)
            next_[i].store(i + 1 < capacity ? static_cast<std::uint32_t>(i + 2) : 0,
                           std::memory_order_relaxed);
        head_.store(1, std::memory_order_relaxed);
    }
    bool try_pop(std::uint32_t& index) noexcept {
        auto head = head_.load(std::memory_order_acquire);
        while (auto encoded = tag_.index(head)) {
            const auto next = next_[encoded - 1].load(std::memory_order_relaxed);
            if (head_.compare_exchange_weak(head, tag_.next(head, next),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                index = encoded - 1;
                return true;
            }
        }
        return false;
    }
    void push(std::uint32_t index) noexcept {
        auto head = head_.load(std::memory_order_relaxed);
        do {
            next_[index].store(tag_.index(head), std::memory_order_relaxed);
        } while (!head_.compare_exchange_weak(head, tag_.next(head, index + 1),
                     std::memory_order_release, std::memory_order_relaxed));
    }
private:
    detail::TaggedIndex tag_;
    std::vector<std::atomic<std::uint32_t>> next_;
    alignas(64) std::atomic<std::uint64_t> head_{0};
};

// Michael–Scott MPMC FIFO 的固定存储索引版本。
// 算法参考：https://www.cs.rochester.edu/research/synchronization/pseudocode/queues.html
// 节点直到队列析构才释放；next/value 全部原子化，head/tail/next 都带版本。
// enqueue 在链接节点时完成发布，其他线程可帮助推进滞后的 tail。
// 没有“先占 ring 序号，再等待该生产者发布”的队头阻塞。
class LockFreeIndexQueue {
public:
    explicit LockFreeIndexQueue(std::size_t capacity)
        : tag_(node_count(capacity)), nodes_(node_count(capacity)), free_(node_count(capacity)) {
        std::uint32_t dummy;
        if (!free_.try_pop(dummy)) std::terminate();
        head_.store(dummy + 1, std::memory_order_relaxed);
        tail_.store(dummy + 1, std::memory_order_relaxed);
    }
    // 暂停在操作中的线程可能暂持节点；false 表示当前无可分配节点。
    // ClaimQueue 的在途槽位预留保证其有效 send/cancel 始终有节点可用。
    bool try_push(std::uint32_t value) noexcept {
        std::uint32_t index;
        if (!free_.try_pop(index)) return false;
        auto& node = nodes_[index];
        node.value.store(value, std::memory_order_relaxed);
        const auto previous = node.next.load(std::memory_order_relaxed);
        node.next.store(tag_.next(previous, 0), std::memory_order_relaxed);
#ifdef BRQ_INDEX_QUEUE_TEST_HOOK
        BRQ_INDEX_QUEUE_TEST_HOOK(1); // 仅确定性并发测试编译启用。
#endif
        auto tail = tail_.load(std::memory_order_acquire);
        for (;;) {
            auto& link = nodes_[tag_.index(tail) - 1].next;
            auto next = link.load(std::memory_order_acquire);
            const auto observed = tail_.load(std::memory_order_acquire);
            if (tail != observed) { tail = observed; continue; }
            if (tag_.index(next)) {
                tail_.compare_exchange_weak(tail, tag_.next(tail, tag_.index(next)),
                    std::memory_order_release, std::memory_order_relaxed);
            } else {
#ifdef BRQ_INDEX_QUEUE_TEST_HOOK
                BRQ_INDEX_QUEUE_TEST_HOOK(5);
#endif
                if (!link.compare_exchange_weak(next, tag_.next(next, index + 1),
                        std::memory_order_release, std::memory_order_relaxed)) {
                    tail = tail_.load(std::memory_order_acquire);
                    continue;
                }
#ifdef BRQ_INDEX_QUEUE_TEST_HOOK
                BRQ_INDEX_QUEUE_TEST_HOOK(2);
#endif
                tail_.compare_exchange_strong(tail, tag_.next(tail, index + 1),
                    std::memory_order_release, std::memory_order_relaxed);
                return true;
            }
            tail = tail_.load(std::memory_order_acquire);
        }
    }
    // order 可选：成功 CAS 时的带版本 head 单调递增，准确对应 FIFO 出队顺序。
    // 不能在出队后另用 fetch_add 编号：两个消费者返回的先后可能与出队先后不同。
    bool try_pop(std::uint32_t& value, std::uint64_t* order = nullptr) noexcept {
        auto head = head_.load(std::memory_order_acquire);
        for (;;) {
            auto tail = tail_.load(std::memory_order_acquire);
            const auto next = nodes_[tag_.index(head) - 1].next.load(std::memory_order_acquire);
            const auto observed = head_.load(std::memory_order_acquire);
            if (head != observed) { head = observed; continue; }
            if (!tag_.index(next)) return false;
            if (tag_.index(head) == tag_.index(tail)) {
                tail_.compare_exchange_weak(tail, tag_.next(tail, tag_.index(next)),
                    std::memory_order_release, std::memory_order_relaxed);
            } else {
                const auto result = nodes_[tag_.index(next) - 1].value.load(std::memory_order_relaxed);
                const auto old_index = tag_.index(head) - 1;
#ifdef BRQ_INDEX_QUEUE_TEST_HOOK
                BRQ_INDEX_QUEUE_TEST_HOOK(4);
#endif
                if (head_.compare_exchange_weak(head, tag_.next(head, tag_.index(next)),
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
#ifdef BRQ_INDEX_QUEUE_TEST_HOOK
                    BRQ_INDEX_QUEUE_TEST_HOOK(3);
#endif
                    // 在将结果交给调用方前归还旧 dummy，保证预留的节点容量。
                    free_.push(old_index);
                    value = result;
                    if (order) *order = head;
                    return true;
                }
            }
            head = head_.load(std::memory_order_acquire);
        }
    }
    [[nodiscard]] bool has_ready() const noexcept {
        for (;;) {
            const auto head = head_.load(std::memory_order_acquire);
            const auto next = nodes_[tag_.index(head) - 1].next.load(std::memory_order_acquire);
            if (head == head_.load(std::memory_order_acquire)) return tag_.index(next) != 0;
        }
    }
private:
    struct Node {
        std::atomic<std::uint64_t> next{0};
        std::atomic<std::uint32_t> value{0};
    };
    static std::size_t node_count(std::size_t capacity) {
        if (!capacity || capacity > std::numeric_limits<std::uint32_t>::max() - 2)
            throw std::invalid_argument("queue_capacity must be in [1, UINT32_MAX - 2]");
        return capacity + 1; // 永久保留一个 dummy 节点。
    }
    detail::TaggedIndex tag_;
    std::vector<Node> nodes_;
    LockFreeIndexStack free_;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
};

} // namespace TwoBufQueue
