#include "TwoBufQueue/src/base_components/HandoffQueue/handoff_queue.hpp"
#include <array>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace TwoBufQueue;
using namespace std::chrono_literals;
struct Item {
    std::size_t value = 0;
    bool reset(std::uint64_t) noexcept { value = 0; return true; }
};
using Pool = WarmPool<Item>;

template<class Predicate> void until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
template<class Pipeline> void drain(Pipeline& pipeline) {
    pipeline.close();
    until([&] { return pipeline.is_drained(); });
}
HandoffOptions synchronous(std::size_t capacity) {
    HandoffOptions options;
    options.mode = HandoffMode::Sync;
    options.queue_capacity = capacity;
    options.enable_stats = true;
    return options;
}

void sync_retry() {
    Pool pool(0, 16);
    bool accept = false;
    std::vector<std::size_t> calls;
    HandoffQueue pipeline(pool, synchronous(3), [&](Item& item) noexcept {
        calls.push_back(item.value);
        return accept;
    });
    auto first = pipeline.try_acquire();
    auto second = pipeline.try_acquire();
    auto third = pipeline.try_acquire();
    assert(first && second && third);
    assert(pipeline.try_acquire().status == AcquireStatus::QueueFull);
    first.loan->value = 1; second.loan->value = 2; third.loan->value = 3;
    auto duplicate = first.loan;
    assert(pipeline.send(first.loan) && !first.loan);
    assert(!pipeline.send(duplicate));
    assert(calls.size() == 1);
    assert(pipeline.send(second.loan));
    assert(calls.size() == 2); // false 后没有再处理另一个元素。
    assert(pipeline.send(third.loan));
    assert(calls.size() == 3);
    assert(pipeline.stats().ready_count == 3);
    accept = true;
    pipeline.retry_pending();
    assert(calls.size() == 6 && pool.all_released());
    assert(pipeline.stats().queue_full == 1);
    drain(pipeline);
}

void limits() {
    Pool pool(0, 1);
    HandoffQueue pipeline(pool, synchronous(3), [](Item&) noexcept { return false; });
    auto one = pipeline.try_acquire();
    assert(one && pipeline.send(one.loan));
    assert(pipeline.try_acquire().status == AcquireStatus::PoolLimit);
    auto received = pipeline.try_receive();
    assert(received && pipeline.stats().received_count == 1);
    assert(pipeline.try_acquire().status == AcquireStatus::PoolLimit);
    assert(received.finish());
    assert(!pipeline.finish(received));
    auto reserved = pipeline.try_acquire();
    pipeline.close();
    assert(pipeline.try_acquire().status == AcquireStatus::Closed);
    assert(!pipeline.is_drained());
    assert(pipeline.send(reserved.loan)); // 关闭前已预留，关闭后仍可提交。
    auto claim = pipeline.try_claim();
    assert(claim && pipeline.finish(claim));
    const auto stats = pipeline.stats();
    assert(stats.pool_limit == 2 && stats.acquire_closed == 1);
    assert(pipeline.is_drained());
}

struct BadFactory {
    Item* create() const { throw std::bad_alloc(); }
    void dispose(Item* item) const noexcept { delete item; }
};
struct BadReset { bool reset(std::uint64_t) noexcept { return false; } };
void errors() {
    WarmPool<Item, true, BadFactory> bad(0, 2);
    HandoffQueue pipeline(bad, synchronous(1), [](Item&) noexcept { return true; });
    for (int i = 0; i < 3; ++i)
        assert(pipeline.try_acquire().status == AcquireStatus::ResourceError);
    assert(pipeline.stats().producing_count == 0);
    assert(pipeline.stats().resource_error == 3);
    drain(pipeline);
    WarmPool<BadReset> reset(0, 2);
    HandoffQueue other(reset, synchronous(1), [](BadReset&) noexcept { return true; });
    assert(other.try_acquire().status == AcquireStatus::ResourceError);
    drain(other);
    Pool ordinary(0, 1);
    PoolGetStatus status;
    auto loan = ordinary.get(status);
    assert(loan && status == PoolGetStatus::Success);
    assert(!ordinary.get(status) && status == PoolGetStatus::Limit);
    assert(ordinary.recycle(loan));
}

void tickets() {
    Pool pool(0, 4, {}, 0); // 强制退役对象，覆盖悬空资源地址的旧凭证。
    HandoffQueue pipeline(pool, synchronous(2), [](Item&) noexcept { return false; });
    auto acquired = pipeline.try_acquire();
    auto old_producer = acquired.loan;
    assert(pipeline.send(acquired.loan));
    auto claim = pipeline.try_claim();
    auto stale = claim;
    assert(pipeline.cancel_claim(claim));
    auto fresh = pipeline.try_claim();
    assert(!pipeline.finish(stale) && !pipeline.cancel_claim(stale));
    std::atomic<int> won{0};
    std::thread a([&, copy = fresh]() mutable { won += pipeline.finish(copy); });
    std::thread b([&, copy = fresh]() mutable { won += pipeline.finish(copy); });
    a.join(); b.join();
    assert(won == 1);
    for (int i = 0; i < 16; ++i) {
        auto next = pipeline.try_acquire();
        assert(next && pipeline.send(next.loan));
        auto received = pipeline.try_receive();
        auto received_copy = received;
        assert(pipeline.finish(received));
        assert(!pipeline.finish(received_copy));
    }
    assert(!pipeline.send(old_producer));
    assert(!pipeline.finish(fresh));
    assert(!pipeline.cancel_claim(stale));
    drain(pipeline);
}

void manual() {
    Pool pool(0, 3);
    HandoffOptions options;
    options.queue_capacity = 2;
    HandoffQueue pipeline(pool, options, [](Item&) noexcept { return true; });
    pipeline.stop();
    auto first = pipeline.try_acquire();
    auto second = pipeline.try_acquire();
    first.loan->value = 1; second.loan->value = 2;
    assert(pipeline.send(second.loan)); // 晚 acquire 的资源可先发布，无头部阻塞。
    auto claim = pipeline.try_claim();
    assert(claim && claim->value == 2);
    assert(pipeline.send(first.loan));
    assert(pipeline.try_acquire().status == AcquireStatus::QueueFull);
    auto received = pipeline.try_receive();
    assert(received && received->value == 1); // receive 跳过已 claim 元素。
    auto third = pipeline.try_acquire();
    assert(third && pipeline.discard(third.loan));
    assert(pipeline.finish(received));
    assert(pipeline.cancel_claim(claim));
    assert(pipeline.start());
    drain(pipeline);
}

void notify_retry(CancelClaimMode cancel_mode = CancelClaimMode::QueueTail) {
    Pool pool(1, 2);
    HandoffOptions options;
    options.queue_capacity = 2;
    options.retry_wait = 200us;
    options.cancel_claim_mode = cancel_mode;
    std::atomic<int> calls{0};
    std::atomic<bool> accept{false};
    HandoffQueue pipeline(pool, options, [&](Item&) noexcept {
        ++calls;
        return accept.load(std::memory_order_acquire);
    });
    auto item = pipeline.try_acquire();
    assert(item && pipeline.send(item.loan));
    until([&] { return calls.load() >= 3; }); // 不再 send，也会自动重试。
    accept.store(true, std::memory_order_release);
    drain(pipeline);
    assert(pool.all_released());
    pipeline.stop();
    assert(!pipeline.is_running());
}

void wake_readers(CancelClaimMode cancel_mode = CancelClaimMode::QueueTail) {
    Pool pool(4, 4);
    HandoffOptions options;
    options.mode = HandoffMode::SPMC;
    options.queue_capacity = 4;
    options.reader_count = 4;
    options.cancel_claim_mode = cancel_mode;
    std::atomic<int> entered{0};
    std::atomic<bool> release{false};
    HandoffQueue pipeline(pool, options, [&](Item&) noexcept {
        ++entered;
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        return true;
    });
    std::this_thread::sleep_for(10ms); // 让所有 Reader 到达等待。
    for (int i = 0; i < 4; ++i) {
        auto item = pipeline.try_acquire();
        assert(item && pipeline.send(item.loan));
    }
    until([&] { return entered.load() == 4; }); // 一批数据确实唤醒了多个消费者。
    release.store(true, std::memory_order_release);
    drain(pipeline);
}

void stress(HandoffMode mode, ReaderMode read_mode, CancelClaimMode cancel_mode = CancelClaimMode::QueueTail) {
    constexpr std::size_t per_producer = 8000;
    const std::size_t producers = mode == HandoffMode::MPSC || mode == HandoffMode::MPMC ? 4 : 1;
    const std::size_t readers = mode == HandoffMode::SPMC || mode == HandoffMode::MPMC ? 4 : 1;
    const auto count = per_producer * producers;
    std::vector<std::atomic<unsigned>> seen(count);
    Pool pool(64, 128);
    HandoffOptions options;
    options.mode = mode;
    options.queue_capacity = 64;
    options.reader_count = readers;
    options.reader_mode = read_mode;
    options.cancel_claim_mode = cancel_mode;
    HandoffQueue pipeline(pool, options, [&](Item& item) noexcept {
        assert(item.value < count);
        const auto previous = seen[item.value].fetch_add(1);
        // 每第七条拒绝一次；同一资源不应同时进入多个回调。
        if (item.value % 7 == 0 && previous == 0) return false;
        assert(previous == (item.value % 7 == 0 ? 1u : 0u));
        return true;
    });
    std::vector<std::thread> threads;
    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            for (std::size_t i = 0; i < per_producer; ++i) {
                auto result = pipeline.try_acquire();
                while (!result) {
                    if (mode == HandoffMode::Sync) pipeline.retry_pending();
                    std::this_thread::yield();
                    result = pipeline.try_acquire();
                }
                result.loan->value = p * per_producer + i;
                assert(pipeline.send(result.loan));
            }
        });
    }
    for (auto& thread : threads) thread.join();
    pipeline.close();
    until([&] {
        if (mode == HandoffMode::Sync) pipeline.retry_pending();
        return pipeline.is_drained();
    });
    for (std::size_t i = 0; i < count; ++i) assert(seen[i] == (i % 7 == 0 ? 2u : 1u));
    assert(pool.all_released());
}

void configuration() {
    Pool pool(0, 4);
    HandoffOptions options;
    options.reader_count = 2;
    bool threw = false;
    try { HandoffQueue pipeline(pool, options, [](Item&) noexcept { return true; }); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    options = synchronous(2);
    HandoffQueue pipeline(pool, options, [](Item&) noexcept { return true; });
    assert(!pipeline.is_running());
    auto item = pipeline.try_acquire();
    assert(item && pipeline.discard(item.loan));
    drain(pipeline);
}

void reentrant() {
    Pool pool(2, 2);
    void* context = nullptr;
    void (*post)(void*) noexcept = nullptr;
    int depth = 0, calls = 0;
    HandoffQueue pipeline(pool, synchronous(2), [&](Item& item) noexcept {
        assert(++depth == 1);
        ++calls;
        if (item.value == 1) post(context);
        --depth;
        return true;
    });
    context = &pipeline;
    post = [](void* value) noexcept {
        auto& owner = *static_cast<decltype(pipeline)*>(value);
        auto nested = owner.try_acquire();
        assert(nested);
        nested.loan->value = 2;
        assert(owner.send(nested.loan));
    };
    auto first = pipeline.try_acquire();
    first.loan->value = 1;
    assert(pipeline.send(first.loan));
    assert(calls == 2);
    drain(pipeline);
}

void foreign() {
    Pool first_pool(0, 2), second_pool(0, 2);
    auto callback = [](Item&) noexcept { return false; };
    HandoffQueue first(first_pool, synchronous(2), callback);
    HandoffQueue second(second_pool, synchronous(2), callback);
    auto item = first.try_acquire();
    assert(!second.send(item.loan));
    assert(first.send(item.loan));
    auto claim = first.try_claim();
    assert(!second.finish(claim) && !second.cancel_claim(claim));
    assert(first.cancel_claim(claim));
    auto received = first.try_receive();
    assert(!second.finish(received));
    assert(first.finish(received));
    drain(first); drain(second);
}

void stop_callback() {
    Pool pool(1, 1);
    HandoffOptions options;
    std::atomic<bool> entered{false}, release{false}, stopped{false};
    HandoffQueue pipeline(pool, options, [&](Item&) noexcept {
        entered.store(true);
        while (!release.load()) std::this_thread::yield();
        return true;
    });
    auto item = pipeline.try_acquire();
    assert(item && pipeline.send(item.loan));
    until([&] { return entered.load(); });
    std::thread stopper([&] { pipeline.stop(); stopped.store(true); });
    std::this_thread::sleep_for(2ms);
    assert(!stopped.load());
    release.store(true);
    stopper.join();
    assert(stopped.load() && pool.all_released());
    drain(pipeline);
}

void sync_concurrent(CancelClaimMode cancel_mode = CancelClaimMode::QueueTail) {
    Pool pool(32, 32);
    std::atomic<unsigned> active{0}, calls{0};
    auto options = synchronous(32);
    options.cancel_claim_mode = cancel_mode;
    HandoffQueue pipeline(pool, options, [&](Item&) noexcept {
        assert(active.fetch_add(1) == 0);
        ++calls;
        assert(active.fetch_sub(1) == 1);
        return true;
    });
    auto produce = [&] {
        for (unsigned i = 0; i < 8000; ++i) {
            auto result = pipeline.try_acquire();
            until([&] {
                if (!result) result = pipeline.try_acquire();
                return bool(result);
            });
            assert(pipeline.send(result.loan));
        }
    };
    std::thread first(produce), second(produce);
    first.join(); second.join();
    pipeline.close();
    // 不额外 retry_pending：否则可能掩盖最后一条并发通知丢失。
    assert(pipeline.is_drained() && calls == 16000);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    std::string test = argv[1];
    const auto cancel_mode = test.starts_with("keep_") ? CancelClaimMode::Keep : CancelClaimMode::QueueTail;
    if (cancel_mode == CancelClaimMode::Keep) test.erase(0, 5);
    if (test == "sync") sync_retry();
    else if (test == "limits") limits();
    else if (test == "errors") errors();
    else if (test == "tickets") tickets();
    else if (test == "manual") manual();
    else if (test == "retry") notify_retry(cancel_mode);
    else if (test == "wake") wake_readers(cancel_mode);
    else if (test == "config") configuration();
    else if (test == "reentrant") reentrant();
    else if (test == "foreign") foreign();
    else if (test == "stop_callback") stop_callback();
    else if (test == "sync_concurrent") sync_concurrent(cancel_mode);
    else {
        auto mode = HandoffMode::Sync;
        if (test.starts_with("spsc")) mode = HandoffMode::SPSC;
        else if (test.starts_with("mpsc")) mode = HandoffMode::MPSC;
        else if (test.starts_with("spmc")) mode = HandoffMode::SPMC;
        else if (test.starts_with("mpmc")) mode = HandoffMode::MPMC;
        else assert(test == "sync_stress");
        stress(mode, test.ends_with("poll") ? ReaderMode::Polling : ReaderMode::WaitSignal, cancel_mode);
    }
    std::cout << "PASS " << argv[1] << '\n';
}
