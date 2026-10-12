#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace TwoBufQueue {

// 单生产者、单消费者的固定容量指针环形队列。
// T 是所指对象类型；只传递非空指针，不创建、释放或接管所指对象。
// 容量在构造时确定，运行时不分配内存。析构前须停止所有队列访问。
template<class T>
class SpscPointerQueue {
public:
    // capacity 是可入队数量；额外一个哨兵槽位用于区分满和空。
    // capacity 为 0 时队列始终为空，所有入队尝试均失败。
    explicit SpscPointerQueue(std::size_t capacity)
        : slots_(slot_count(capacity), nullptr) {}

    SpscPointerQueue(const SpscPointerQueue&) = delete;
    SpscPointerQueue& operator=(const SpscPointerQueue&) = delete;

    // 仅生产者调用；value 必须非空。队列满时返回 false，保持原状态。
    [[nodiscard]] bool try_push(T* value) noexcept {
        assert(value != nullptr);
        const std::size_t write = write_.load(std::memory_order_relaxed);
        const std::size_t next = (write + 1) % slots_.size();
        if (next == read_.load(std::memory_order_acquire)) return false;
        slots_[write] = value;
        write_.store(next, std::memory_order_release);
        return true;
    }

    // 仅消费者调用；查看队首但不出队，空队列返回 nullptr。
    [[nodiscard]] T* front() const noexcept {
        const std::size_t read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return nullptr;
        return slots_[read];
    }

    // 仅消费者调用，且队列必须非空；只释放槽位，不释放所指对象。
    // 可先用 front() 处理对象，处理成功后再 pop()。
    void pop() noexcept {
        assert(!empty());
        const std::size_t read = read_.load(std::memory_order_relaxed);
        slots_[read] = nullptr;
        read_.store((read + 1) % slots_.size(), std::memory_order_release);
    }

    // 仅消费者调用，与 front() 使用相同的空队列判断。
    [[nodiscard]] bool empty() const noexcept { return front() == nullptr; }

    // 可由任意线程查询；并发时为近似快照，不能代替 try_push/front 的判断。
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t read = read_.load(std::memory_order_acquire);
        const std::size_t write = write_.load(std::memory_order_acquire);
        return write >= read ? write - read : slots_.size() - read + write;
    }

private:
    static std::size_t slot_count(std::size_t capacity) {
        // 必须先检查再加一，避免哨兵槽位数量溢出。
        if (capacity == std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("SpscPointerQueue capacity overflow");
        }
        return capacity + 1;
    }

    std::vector<T*> slots_;
    // 读写下标分隔到不同缓存行，沿用原队列的布局。
    alignas(64) std::atomic<std::size_t> read_{0};
    alignas(64) std::atomic<std::size_t> write_{0};
};

}  // namespace TwoBufQueue
