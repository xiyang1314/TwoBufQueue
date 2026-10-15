#include "../../support/joining_thread.hpp"
// HandoffQueue in MPMC mode: multiple producers send; built-in Reader threads invoke callbacks concurrently.
// Consumer implements business processing only; no extra reader thread. Accumulate shared results with atomics.

#include "TwoBufQueue/src/base_components/handoff_queue/handoff_queue.hpp"
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
    // true completes processing; this example does not demonstrate cancellation and retry after false.
    bool process(Message& message) noexcept {
        total_.fetch_add(message.value, std::memory_order_relaxed);
        return true; // The built-in Reader then calls finish to return the resource.
    }
    int total() const noexcept { return total_.load(std::memory_order_relaxed); }

private:
    std::atomic<int> total_{0};
};

// Retry only capacity backpressure; other acquisition errors terminate this example's thread.
template<class TQueue>
auto acquire_until_available(TQueue& queue) {
    for (;;) {
        auto acquired = queue.try_acquire();
        if (acquired) return acquired;
        if (acquired.status != AcquireStatus::QueueFull &&
            acquired.status != AcquireStatus::PoolLimit) std::terminate();
        std::this_thread::yield();
    }
}

int main() try {
    //---------------1. Initialize the pool, consumer and queue--------------
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
    HandoffQueue queue(pool, options, callback); // Construction starts reader_count reader threads.

    //---------------2. Each producer borrows, writes and sends--------------
    {
        std::vector<brq_support::JoiningThread> producers;
        for (int i = 0; i < producer_count; ++i) {
            producers.emplace_back([&queue] {
                for (int value = 1; value <= per_producer; ++value) {
                    auto acquired = acquire_until_available(queue);
                    acquired.loan->value = value;
                    if (!queue.send(acquired.loan)) std::terminate();
                    // Do not access the resource after send.
                }
            });
        }
    } // Destruction of jthread waits for every producer to finish.

    //---------------3. Drain and verify------------------------------------
    queue.close();
    while (!queue.is_drained()) std::this_thread::yield();
    queue.stop(); // Wait until the built-in Reader threads exit.

    const int expected = producer_count * (per_producer * (per_producer + 1) / 2);
    if (consumer.total() != expected || !pool.all_released())
        throw std::runtime_error("delivery mismatch");
    std::puts("PASS: 2 producers + 2 consumers, 200 records summed");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
