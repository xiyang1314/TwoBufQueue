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

// 通用单生产者、单消费者 FIFO 队列，不依赖日志、WriterBlock<> 或回调。
// 容量在构造时确定，运行中队列自身不分配内存；T 的构造/析构开销由 T 决定。
// 元素入队时构造，pop 时析构；支持非默认构造类型和只能移动的类型。
// 析构前必须停止生产者和消费者；front 返回的指针只在 pop 前有效。
template<class T>
class SpscQueue {
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "SpscQueue requires lock-free atomic indices");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "SpscQueue requires a non-throwing element destructor");

public:
    // capacity 为实际可容纳的元素数，额外一个槽位用于区分满和空。
    // 允许零容量：始终为空，所有入队均返回 false。
    explicit SpscQueue(std::size_t capacity)
        : slot_count_(checked_slot_count(capacity)),
          slots_(std::make_unique<Slot[]>(slot_count_)) {}

    ~SpscQueue() noexcept {
        // 此时两端已经停止，仅销毁仍在队列中的对象。
        auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_relaxed);
        while (read != write) {
            std::destroy_at(element(read));
            read = next_index(read);
        }
    }
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // 仅生产者调用；满时不构造元素、不移动参数，也不改变队列状态。
    // T 构造抛异常时写下标不发布，因此消费者不会看到未构造完的对象。
    template<class... Args>
    [[nodiscard]] bool try_emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = next_index(write);
        if (next == read_.load(std::memory_order_acquire)) return false;
        std::construct_at(reinterpret_cast<T*>(slots_[write].storage),
                          std::forward<Args>(args)...);
        write_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_push(const T& value)
        noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return try_emplace(value);
    }
    [[nodiscard]] bool try_push(T&& value)
        noexcept(std::is_nothrow_move_constructible_v<T>) {
        return try_emplace(std::move(value));
    }

    // 仅消费者调用；查看但不出队。即使 T 是空指针，槽位地址仍然非空。
    [[nodiscard]] const T* front() const noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return nullptr;
        return element(read);
    }
    [[nodiscard]] T* front() noexcept {
        return const_cast<T*>(std::as_const(*this).front());
    }

    // 仅消费者调用：查看距队首 offset 个位置的元素，不出队。
    // 用于先收集一批再统一处理；消费者 pop 前，生产者不能覆盖这些活跃槽位。
    // 越界返回 nullptr；调用方应先固定本轮数量，避免持续追逐生产者的新入队。
    [[nodiscard]] const T* front(std::size_t offset) const noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        const auto count = write >= read ? write - read : slot_count_ - read + write;
        if (offset >= count) return nullptr;
        // 不先做 read + offset，避免容量极大时无符号加法溢出。
        const auto tail = slot_count_ - read;
        return element(offset < tail ? read + offset : offset - tail);
    }

    // 仅消费者调用，且队列必须非空；业务处理成功后才调用。
    // 先销毁旧元素，再发布读下标，生产者随后才能复用该槽位。
    void pop() noexcept {
        assert(!empty());
        const auto read = read_.load(std::memory_order_relaxed);
        std::destroy_at(element(read));
        read_.store(next_index(read), std::memory_order_release);
    }

    // empty 与 front 一样只供消费者判断；size 可供任意线程读取近似快照。
    [[nodiscard]] bool empty() const noexcept { return front() == nullptr; }
    [[nodiscard]] std::size_t size() const noexcept {
        const auto read = read_.load(std::memory_order_acquire);
        const auto write = write_.load(std::memory_order_acquire);
        return write >= read ? write - read : slot_count_ - read + write;
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return slot_count_ - 1; }

private:
    // 只预留对齐的存储，不要求 T 默认构造；活跃对象的生命周期由队列显式管理。
    struct Slot { alignas(T) std::byte storage[sizeof(T)]; };

    static std::size_t checked_slot_count(std::size_t capacity) {
        if (capacity == std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("SpscQueue capacity overflow");
        }
        return capacity + 1;
    }
    [[nodiscard]] std::size_t next_index(std::size_t index) const noexcept {
        return (index + 1) % slot_count_;
    }
    // 仅在槽位内已有完整 T 对象时调用。
    [[nodiscard]] T* element(std::size_t index) noexcept {
        return std::launder(reinterpret_cast<T*>(slots_[index].storage));
    }
    [[nodiscard]] const T* element(std::size_t index) const noexcept {
        return std::launder(reinterpret_cast<const T*>(slots_[index].storage));
    }

    const std::size_t slot_count_;
    std::unique_ptr<Slot[]> slots_;
    // 两端各自修改的下标分开缓存行，避免无谓的相互失效。
    alignas(64) std::atomic<std::size_t> read_{0};
    alignas(64) std::atomic<std::size_t> write_{0};
};

}  // namespace TwoBufQueue
