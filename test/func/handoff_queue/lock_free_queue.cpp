#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
static void pause_operation(int point);
#define BRQ_INDEX_QUEUE_TEST_HOOK(point) pause_operation(point)
#include "TwoBufQueue/src/base_components/HandoffQueue/pooled_resource_queue.hpp"
#undef BRQ_INDEX_QUEUE_TEST_HOOK

using namespace TwoBufQueue;
static std::atomic<bool> paused{false}, resume{false};
static thread_local int pause_at = 0;
static void pause_operation(int point) {
    if (pause_at != point) return;
    pause_at = 0;
    paused.store(true, std::memory_order_release);
    while (!resume.load(std::memory_order_acquire)) std::this_thread::yield();
}
static void await_pause() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!paused.load(std::memory_order_acquire)) {
        assert(std::chrono::steady_clock::now() < until);
        std::this_thread::yield();
    }
}
// 真实暂停在算法内部，其他线程必须能完成，不只是检查有没有 lock 关键字。
static void stalled(int point) {
    LockFreeIndexQueue queue(3);
    paused = false; resume = false;
    if (point == 3 || point == 4) assert(queue.try_push(11));
    std::uint32_t held = 0;
    std::thread thread([&] {
        pause_at = point;
        if (point == 3 || point == 4) {
            if (!queue.try_pop(held)) held = 0;
        } else assert(queue.try_push(11));
    });
    await_pause();
    std::uint32_t value;
    if (point == 2 || point == 4) { assert(queue.try_pop(value)); assert(value == 11); }
    // cap=3，循环强制反复复用节点，检测旧 head / 旧 next 的 ABA 防护。
    for (std::uint32_t i = 20; i < 30020; ++i) {
        assert(queue.try_push(i));
        assert(queue.try_pop(value));
        assert(value == i);
    }
    resume.store(true, std::memory_order_release);
    thread.join();
    if (point == 1 || point == 5) { assert(queue.try_pop(value)); assert(value == 11); }
    if (point == 3) assert(held == 11);
    if (point == 4) assert(held == 0);
    assert(!queue.try_pop(value));
}
static void stress(std::size_t capacity, std::size_t producers, std::size_t consumers) {
    constexpr std::size_t count = 30000;
    LockFreeIndexQueue queue(capacity);
    std::vector<std::atomic<unsigned>> seen(count * producers);
    std::atomic<std::size_t> remaining{producers};
    std::vector<std::thread> threads;
    for (std::size_t p = 0; p < producers; ++p) threads.emplace_back([&, p] {
        for (std::size_t i = 0; i < count; ++i)
            while (!queue.try_push(static_cast<std::uint32_t>(p * count + i))) std::this_thread::yield();
        remaining.fetch_sub(1, std::memory_order_release);
    });
    for (std::size_t c = 0; c < consumers; ++c) threads.emplace_back([&] {
        std::vector<std::size_t> next(producers);
        for (;;) {
            std::uint32_t value;
            if (queue.try_pop(value)) {
                assert(value < count * producers);
                assert(seen[value].fetch_add(1, std::memory_order_relaxed) == 0);
                if (consumers == 1) assert(value % count == next[value / count]++);
            } else if (!remaining.load(std::memory_order_acquire) && !queue.has_ready()) break;
            else std::this_thread::yield();
        }
    });
    for (auto& thread : threads) thread.join();
    for (auto& value : seen) assert(value.load() == 1);
}
struct Item {
    int value = 0;
    bool reset(std::uint64_t) noexcept { value = 0; return true; }
};
static void held_slot() {
    WarmPool<Item> pool(2, 2);
    PooledResourceQueue queue(pool, 2);
    auto held = queue.try_acquire();
    assert(held);
    const auto progress = [&] {
        for (int i = 0; i < 30000; ++i) {
            auto producer = queue.try_acquire();
            assert(producer);
            producer.loan->value = i;
            assert(queue.send(producer.loan));
            auto claim = queue.try_claim();
            assert(claim && claim->value == i);
            assert(queue.finish(claim));
        }
    };
    progress(); // 一个生产者暂停，其他已就绪资源仍能前进。
    assert(queue.send(held.loan));
    auto claim = queue.try_claim();
    assert(claim);
    progress(); // 一个消费者暂停，不阻塞其他槽位。
    assert(queue.cancel_claim(claim));
    claim = queue.try_claim();
    assert(queue.finish(claim));
    queue.close();
    assert(queue.is_drained());
}
static void close_race() {
    WarmPool<Item> pool(8, 8);
    PooledResourceQueue queue(pool, 8);
    std::atomic<unsigned> submitted{0}, completed{0};
    std::vector<std::thread> threads;
    for (int p = 0; p < 4; ++p) threads.emplace_back([&] {
        for (;;) {
            auto producer = queue.try_acquire();
            if (producer.status == AcquireStatus::Closed) return;
            if (producer) { assert(queue.send(producer.loan)); submitted.fetch_add(1); }
            else std::this_thread::yield();
        }
    });
    for (int c = 0; c < 2; ++c) threads.emplace_back([&] {
        while (!queue.is_drained()) {
            if (auto claim = queue.try_claim()) { assert(queue.finish(claim)); completed.fetch_add(1); }
            else std::this_thread::yield();
        }
    });
    while (submitted.load() < 1000) std::this_thread::yield();
    queue.close();
    for (auto& thread : threads) thread.join();
    assert(submitted == completed);
    assert(queue.is_drained() && pool.all_released());
}
int main() {
    for (auto capacity : {std::size_t{0}, std::size_t{std::numeric_limits<std::uint32_t>::max() - 1}}) {
        bool rejected = false;
        try { ClaimQueue<Item> queue(capacity); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    for (int point = 1; point <= 5; ++point) stalled(point);
    for (std::size_t capacity : {1, 2, 3, 64}) {
        stress(capacity, 1, 1);
        stress(capacity, 4, 1);
        stress(capacity, 1, 4);
        stress(capacity, 4, 4);
    }
    held_slot();
    close_race();
    std::cout << "PASS lock_free_queue (5 pause points, 16 stress configurations, held slot, close race)\n";
}
