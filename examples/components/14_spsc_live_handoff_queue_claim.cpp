// SpscLiveHandoffQueue：claim/cancel 方式
// claim/cancel相比pop的优点是：消费者线程处理会更简单些，不需要自己pending一个产品；当消费者线程异常结束的时候，不会产生资源泄漏

#include "TwoBufQueue/src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp"
#include "TwoBufQueue/src/utils/auto_reset_event.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
using namespace TwoBufQueue;

struct Message {
    int value = 0;
    std::atomic<bool> ready{false};
    bool consumed = false; // 仅消费端访问。

    bool reset(std::uint64_t) noexcept {
        value = 0;
        consumed = false;
        ready.store(false, std::memory_order_relaxed);
        return true;
    }
    void publish(int new_value) noexcept {
        value = new_value;
        ready.store(true, std::memory_order_release);
    }
};

using Queue = SpscLiveHandoffQueue<Message>;

//消费者线程
class Consumer {
public:
    explicit Consumer(Queue& queue)
        : queue_(queue), thread_([this] {
            for (;;) {
                wake_.wait();
                if (stopping_.load(std::memory_order_acquire)) return;
                consume();
            }
        }) {
        queue_.set_read_available_callback(&Consumer::queue_notify, this);
    }

    ~Consumer() {
        queue_.set_read_available_callback(nullptr, nullptr);
        stopping_.store(true, std::memory_order_release);
        wake_.set(); thread_.join();
    }

    // 队列回调入口：唤醒消费者，不等待扫描结束。
    void notify() { wake_.set(); }
    void allow_processing() noexcept { can_process_.store(true, std::memory_order_release); }
    int cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }
    int current_processed() const noexcept { return current_processed_.load(std::memory_order_acquire); }

private:
    static void queue_notify(void* context) noexcept {
        static_cast<Consumer*>(context)->notify();
    }
    // 消费线程执行：先遍历本轮完成记录，全部处理成功后才处理本轮 current。
    void consume() {
        std::size_t finish_count = 0;
        // 有记录：取本轮最后一条的 next；没有记录：取 queue.current()。
        const auto current = queue_.snapshot_for_read(finish_count);

        for (std::size_t i = 0; i < finish_count; ++i) {
            auto claim = queue_.try_claim();
            assert(claim); // 本轮固定的队列前缀不会被其他消费者取走。

            if (claim.get() && !process(*claim, false)) {
                queue_.cancel_claim(claim);
                cancelled_.fetch_add(1, std::memory_order_release);
                return; // 保留队首记录，下次再处理。
            }
            queue_.finish(claim); // 出队；有完成资源时归还，之后不可再访问。
        }
        // 处理期间新入队的记录留给下一轮；这里不再重新取队尾或生产端 current。
        if (current) (void)process(*current, true); // 只读当前块，不能 finish。
    }

    // true 表示当前没有待处理数据或已处理；false 表示条件不足，下次再读。
    bool process(Message& message, bool current) {
        if (!message.ready.load(std::memory_order_acquire)) return current; // 完成块未发布时不能 finish。
        if (message.consumed) return true;
        if (!can_process_.load(std::memory_order_acquire)) return false;
        std::printf("%s=%d\n", current ? "current" : "completed", message.value);
        message.consumed = true;
        if (current) current_processed_.fetch_add(1, std::memory_order_release);
        return true;
    }

    Queue& queue_;
    std::atomic<bool> can_process_{false};
    std::atomic<int> cancelled_{0};
    std::atomic<int> current_processed_{0};
    AutoResetEvent wake_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;   // 最后构造，确保线程访问的成员已经初始化。
};

// 生产者侧辅助函数：修改共享数据后通知消费者。
template<class Modify>
void modify_and_notify(Queue& queue, Modify&& modify) {
    modify();
    queue.notify_read_available();
}

// 示例中简单重试直到拿到资源；实际调用方需要按状态自行处理超时、背压等问题。
Queue::AcquireResult acquire_until_available(Queue& queue) {
    auto result = queue.try_acquire();
    while (!result) {
        if (result.status != LiveAcquireStatus::QueueFull &&
            result.status != LiveAcquireStatus::PoolLimit)
            throw std::runtime_error("acquire failed");
        std::this_thread::yield();
        result = queue.try_acquire();
    }
    return result;
}

int main() try {
    //--------------- 1. 初始化：资源池 + 顺序交接队列（容量 16）----------------
    constexpr std::size_t pool_min = 2, pool_max = 8, queue_capacity = 16;
    Queue queue(pool_min, pool_max, queue_capacity);
    {
        Consumer consumer(queue); // 消费线程独立运行；主线程负责生产。

        //--------------- 2. A 尚未 send，消费者也可读取 current ----------------
        auto acquired = acquire_until_available(queue); // 入队 {空, A} 开始记录。
        modify_and_notify(queue, [&] { acquired.loan->publish(7); }); // 通知消费者读取 current=7。

        //--------------- 3. 完成 A，换到 B -------------------------------
        acquired = queue.send_and_acquire();            // 入队 {A, B}；之后不能再访问 A。
        if (!acquired) acquired = acquire_until_available(queue); // 旧块已发送；循环申请新块。

        // A 已完成但处理条件不足：先确认消费者确实 cancel 了一次。
        while (consumer.cancelled() == 0) std::this_thread::yield();
        consumer.allow_processing();
        modify_and_notify(queue, [&] { acquired.loan->publish(9); }); // 重试 A，再读取当前块 B。
        while (consumer.current_processed() == 0) std::this_thread::yield();

        //--------------- 4. 全部完成 -------------------------------
        if (!queue.send()) throw std::runtime_error("send failed"); // {B, 空}。
        queue.close();
        while (!queue.all_released()) std::this_thread::yield(); // notify 不等待；这里等资源回池。
    } // 停止并 join 消费线程，之后才能销毁队列和池。
    if (!queue.is_drained() || !queue.all_released()) throw std::runtime_error("drain failed");
    std::puts("PASS: current read, claim/cancel retry and resource return");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
