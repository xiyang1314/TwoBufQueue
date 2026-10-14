// HandoffQueue in SPSC mode: the main thread produces; one built-in Reader invokes the consumer callback.

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

// HandoffQueue's built-in Reader thread calls this consumer logic.
class Consumer {
public:
    // true completes processing; false makes the Reader cancel its claim and retry later.
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

// Retry only capacity backpressure in this example; report other acquisition errors to the caller.
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
    //---------------1. Initialize the pool, consumer and queue--------------
    constexpr std::size_t pool_min = 2, pool_max = 8, queue_capacity = 8;
    WarmPool<Message> pool(pool_min, pool_max);
    Consumer consumer;

    HandoffOptions options;
    options.mode = HandoffMode::SPSC;
    // For single-threaded debugging, use HandoffMode::Sync: no Reader thread; send()/retry_pending() invoke the callback in their calling thread.
    options.queue_capacity = queue_capacity;
    options.cancel_claim_mode = CancelClaimMode::Keep;
    auto callback = [&consumer](Message& message) noexcept { return consumer.process(message); };
    HandoffQueue queue(pool, options, callback);

    //---------------2. Abandon an unsent resource---------------------------
    auto unused = acquire_until_available(queue);
    if (!queue.discard(unused.loan)) throw std::runtime_error("discard failed");

    //---------------3. Produce: borrow, write and send----------------------
    for (int value : {10, 20, 30}) {
        auto acquired = acquire_until_available(queue);
        acquired.loan->value = value;
        if (!queue.send(acquired.loan)) throw std::runtime_error("send failed");
        // send notifies the Reader; the consumer callback runs on the Reader thread.
    }

    //---------------4. Once ready, let the Reader retry and drain-----------
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
