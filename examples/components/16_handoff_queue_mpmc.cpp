#include "../../support/joining_thread.hpp"
// HandoffQueue：MPMC 模式。多个生产者 send；队列自带多个 Reader 线程并发调用处理回调。
// Consumer 只实现业务处理，不另建读取线程；共享结果用原子变量累计。

#include "TwoBufQueue/src/base_components/HandoffQueue/handoff_queue.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace TwoBufQueue;

struct Message {
    int value = 0;
    bool reset(std::uint64_t) noexcept { value = 0; return true; }
};

class Consumer {
public:
    // true 表示处理完成；本例不演示 false 后的取消与重试。
    bool process(Message& message) noexcept {
        total_.fetch_add(message.value, std::memory_order_relaxed);
        return true; // 内置 Reader 随后 finish，归还资源。
    }
    int total() const noexcept { return total_.load(std::memory_order_relaxed); }

private:
    std::atomic<int> total_{0};
};

// 示例中仅重试容量背压；线程函数里的其他申请错误直接终止示例。
template<class Queue>
auto acquire_until_available(Queue& queue) {
    for (;;) {
        auto acquired = queue.try_acquire();
        if (acquired) return acquired;
        if (acquired.status != AcquireStatus::QueueFull &&
            acquired.status != AcquireStatus::PoolLimit) std::terminate();
        std::this_thread::yield();
    }
}

int main() try {
    //--------------- 1. 初始化：资源池、消费者和队列 ---------------------------
    constexpr std::size_t pool_min = 2, pool_max = 16, queue_capacity = 32, reader_count = 2;
    constexpr int producer_count = 2, per_producer = 100;
    WarmPool<Message> pool(pool_min, pool_max);
    Consumer consumer;

    HandoffOptions options;
    options.mode = HandoffMode::MPMC;
    options.queue_capacity = queue_capacity;
    options.reader_count = reader_count;
    options.reader_mode = ReaderMode::WaitSignal;
    options.cancel_claim_mode = CancelClaimMode::Keep;
    auto callback = [&consumer](Message& message) noexcept { return consumer.process(message); };
    HandoffQueue queue(pool, options, callback); // 构造时启动 reader_count 个读取线程。

    //--------------- 2. 多个生产者各自借资源、写入、发送 -----------------------
    {
        std::vector<brq_support::JoiningThread> producers;
        for (int i = 0; i < producer_count; ++i) {
            producers.emplace_back([&queue] {
                for (int value = 1; value <= per_producer; ++value) {
                    auto acquired = acquire_until_available(queue);
                    acquired.loan->value = value;
                    if (!queue.send(acquired.loan)) std::terminate();
                    // send 后不再访问该资源。
                }
            });
        }
    } // jthread 析构时等待全部生产者结束。

    //--------------- 3. 排空并校验 -------------------------------------------
    queue.close();
    while (!queue.is_drained()) std::this_thread::yield();
    queue.stop(); // 等待内置 Reader 线程退出。

    const int expected = producer_count * (per_producer * (per_producer + 1) / 2);
    if (consumer.total() != expected || !pool.all_released())
        throw std::runtime_error("delivery mismatch");
    std::puts("PASS: 2 producers + 2 consumers, 200 records summed");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
