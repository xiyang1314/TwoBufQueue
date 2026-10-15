// SpscLiveHandoffQueue: the claim/cancel interface.
// Compared with pop, claim/cancel keeps pending resources in the queue, simplifies retries and avoids losing a popped resource if the consumer exits unexpectedly.

#include "TwoBufQueue/src/base_components/live_handoff_queue/spsc_live_handoff_queue.hpp"
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
    bool consumed = false; // Accessed only by the consumer.

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

// Consumer thread.
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

    // Queue callback: wake the consumer without waiting for its scan to finish.
    void notify() { wake_.set(); }
    void allow_processing() noexcept { can_process_.store(true, std::memory_order_release); }
    int cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }
    int current_processed() const noexcept { return current_processed_.load(std::memory_order_acquire); }

private:
    static void queue_notify(void* context) noexcept {
        static_cast<Consumer*>(context)->notify();
    }
    // Consumer thread: process this round's completed records before this round's current resource.
    void consume() {
        std::size_t finish_count = 0;
        // With records, use the last record's next; otherwise use queue.current().
        const auto current = queue_.snapshot_for_read(finish_count);

        for (std::size_t i = 0; i < finish_count; ++i) {
            auto claim = queue_.try_claim();
            assert(claim); // No other consumer can remove this round's fixed queue prefix.

            if (claim.get() && !process(*claim, false)) {
                queue_.cancel_claim(claim);
                cancelled_.fetch_add(1, std::memory_order_release);
                return; // Leave the head record in place for a later retry.
            }
            queue_.finish(claim); // Dequeue and return any completed resource; do not access it afterward.
        }
        // Records enqueued during processing belong to the next round; do not reload the tail or producer current here.
        if (current) (void)process(*current, true); // Read the current block only; do not finish it.
    }

    // true means no pending data or handled; false means conditions are not ready, so retry later.
    bool process(Message& message, bool current) {
        if (!message.ready.load(std::memory_order_acquire)) return current; // Do not finish a block whose completion has not been published.
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
    std::thread thread_;   // Construct last so every member accessed by the thread is already initialized.
};

// Producer helper: notify the consumer after changing shared data.
template<class TModify>
void modify_and_notify(Queue& queue, TModify&& modify) {
    modify();
    queue.notify_read_available();
}

// This example retries until a resource is available. Applications must handle timeouts, backpressure and other states.
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
    //---------------1. Initialize a pool and ordered handoff queue (16)------
    constexpr std::size_t pool_min = 2, pool_max = 8, queue_capacity = 16;
    Queue queue(pool_min, pool_max, queue_capacity);
    {
        Consumer consumer(queue); // The consumer runs independently; the main thread produces data.

        //---------------2. Read current before A is sent------------------------
        auto acquired = acquire_until_available(queue); // Enqueue the {empty, A} begin record.
        modify_and_notify(queue, [&] { acquired.loan->publish(7); }); // Notify the consumer to read current = 7.

        //---------------3. Complete A and move to B-----------------------------
        acquired = queue.send_and_acquire();            // Enqueue {A, B}; do not access A afterward.
        if (!acquired) acquired = acquire_until_available(queue); // The old block has been sent; keep trying to acquire the next block.

        // A is complete but cannot be processed yet; first verify that the consumer cancelled once.
        while (consumer.cancelled() == 0) std::this_thread::yield();
        consumer.allow_processing();
        modify_and_notify(queue, [&] { acquired.loan->publish(9); }); // Retry A, then read the current block B.
        while (consumer.current_processed() == 0) std::this_thread::yield();

        //---------------4. Finish all work-------------------------------------
        if (!queue.send()) throw std::runtime_error("send failed"); // {B, empty}.
        queue.close();
        while (!queue.all_released()) std::this_thread::yield(); // notify does not wait; wait here until the resource returns to the pool.
    } // Stop and join the consumer before destroying the queue and pool.
    if (!queue.is_drained() || !queue.all_released()) throw std::runtime_error("drain failed");
    std::puts("PASS: current read, claim/cancel retry and resource return");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
