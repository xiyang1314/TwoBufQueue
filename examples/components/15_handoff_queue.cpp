// HandoffQueue：SPSC 模式。主线程生产，队列的一个内置 Reader 线程调用处理回调。

#include "TwoBufQueue/src/base_components/HandoffQueue/handoff_queue.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
using namespace TwoBufQueue;

struct Message {
    int value = 0;
    bool reset(std::uint64_t) noexcept { value = 0; return true; }
};

// 消费逻辑由 HandoffQueue 的内置 Reader 线程调用。
class Consumer {
public:
    // true 表示处理完成；false 表示条件不足，由 Reader 取消领取、稍后重试。
    bool process(Message& message) noexcept {
        if (!can_process_.load(std::memory_order_acquire)) {
            deferred_attempts_.fetch_add(1, std::memory_order_release);
            std::printf("defer value=%d\n", message.value);
            return false;
        }
        total_.fetch_add(message.value, std::memory_order_relaxed);
        received_.fetch_add(1, std::memory_order_relaxed);
        std::printf("processed value=%d\n", message.value);
        return true;
    }
    void allow_processing() noexcept { can_process_.store(true, std::memory_order_release); }
    int deferred_attempts() const noexcept { return deferred_attempts_.load(std::memory_order_acquire); }
    int received() const noexcept { return received_.load(std::memory_order_acquire); }
    int total() const noexcept { return total_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> can_process_{false};
    std::atomic<int> deferred_attempts_{0};
    std::atomic<int> received_{0};
    std::atomic<int> total_{0};
};

// 示例中仅重试容量背压；其他申请错误直接报告给调用方。
template<class Queue>
auto acquire_until_available(Queue& queue) {
    for (;;) {
        auto acquired = queue.try_acquire();
        if (acquired) return acquired;
        if (acquired.status != AcquireStatus::QueueFull &&
            acquired.status != AcquireStatus::PoolLimit)
            throw std::runtime_error("acquire failed");
        std::this_thread::yield();
    }
}

int main() try {
    //--------------- 1. 初始化：资源池、消费者和队列 ---------------------------
    constexpr std::size_t pool_min = 2, pool_max = 8, queue_capacity = 8;
    WarmPool<Message> pool(pool_min, pool_max);
    Consumer consumer;

    HandoffOptions options;
    options.mode = HandoffMode::SPSC;
    // 单线程调试可改为 HandoffMode::Sync：不创建 Reader 线程，send()/retry_pending() 在调用它们的线程执行消费回调。
    options.queue_capacity = queue_capacity;
    options.cancel_claim_mode = CancelClaimMode::Keep;
    auto callback = [&consumer](Message& message) noexcept { return consumer.process(message); };
    HandoffQueue queue(pool, options, callback);

    //--------------- 2. 放弃一条尚未发送的资源 -------------------------------
    auto unused = acquire_until_available(queue);
    if (!queue.discard(unused.loan)) throw std::runtime_error("discard failed");

    //--------------- 3. 生产：借资源、写入、发送 -------------------------------
    for (int value : {10, 20, 30}) {
        auto acquired = acquire_until_available(queue);
        acquired.loan->value = value;
        if (!queue.send(acquired.loan)) throw std::runtime_error("send failed");
        // send 通知 Reader；消费回调在 Reader 线程执行。
    }

    //--------------- 4. 条件满足后，由 Reader 重试并排空 -----------------------
    while (consumer.deferred_attempts() == 0) std::this_thread::yield();
    const bool deferred = consumer.received() == 0;
    consumer.allow_processing();
    queue.close();
    while (!queue.is_drained()) std::this_thread::yield();
    queue.stop();

    if (!deferred || consumer.received() != 3 || consumer.total() != 60 ||
        !pool.all_released()) throw std::runtime_error("delivery mismatch");
    std::puts("PASS: discard, callback retry, 3 messages summed to 60");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
