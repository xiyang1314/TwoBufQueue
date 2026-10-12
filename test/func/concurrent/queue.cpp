#include "../../../support/joining_thread.hpp"
#include "common.hpp"
namespace ct {
void queue_run(std::size_t capacity) {
    SpscQueue<std::unique_ptr<Bytes>> queue(capacity);
    Failure failure;
    constexpr unsigned count = 20001;
    std::atomic<unsigned> full{0};
    std::atomic<bool> initial_full{false};
    std::barrier start(2);
    brq_support::JoiningThread producer([&] {
        start.arrive_and_wait();
        failure.run([&] {
            const auto deadline = Clock::now() + 30s;
            for (unsigned seq = 0; seq < count && !failure.failed.load(); ++seq) {
                auto value = std::make_unique<Bytes>(message(0, seq, 16 + seq % 241));
                const auto expected = *value;
                while (!queue.try_push(std::move(value))) {
                    CHECK(value);
                    equal_bytes(value->data(), value->size(), expected, "full queue source");
                    full.fetch_add(1);
                    initial_full = true;
                    if (failure.failed.load())
                        return;
                    if (Clock::now() > deadline)
                        throw std::runtime_error("queue producer deadline");
                    std::this_thread::yield();
                }
                CHECK(!value);
            }
        });
    });
    start.arrive_and_wait();
    failure.run([&] {
        const auto deadline = Clock::now() + 30s;
        wait_flag(initial_full, failure, deadline); // Guarantee the full path is exercised.
        for (unsigned seq = 0; seq < count && !failure.failed.load(); ++seq) {
            auto *slot = queue.front();
            while (!slot) {
                if (failure.failed.load())
                    return;
                if (Clock::now() > deadline)
                    throw std::runtime_error("queue consumer deadline");
                std::this_thread::yield();
                slot = queue.front();
            }
            CHECK(*slot);
            const auto expected = message(0, seq, 16 + seq % 241);
            equal_bytes((*slot)->data(), (*slot)->size(), expected,
                        "queue front seq=" + std::to_string(seq));
            if (seq % 17 == 0) {
                std::this_thread::yield();
                CHECK(queue.front() == slot);
                equal_bytes((*slot)->data(), (*slot)->size(), expected, "queue front before pop");
            }
            auto owned = std::move(*slot);
            queue.pop();
            equal_bytes(owned->data(), owned->size(), expected, "moved after pop");
        }
    });
    producer.join();
    failure.check();
    CHECK(queue.empty());
    CHECK(full.load() > 0);
    std::cout << "verified FIFO messages=" << count << " full retries=" << full << '\n';
}
void queue_cases(Cases &cases) {
    for (std::size_t capacity : {1u, 2u, 3u, 7u, 64u, 65u})
        cases.push_back({"queue_" + std::to_string(capacity), [=] { queue_run(capacity); }});
}
} // namespace ct
