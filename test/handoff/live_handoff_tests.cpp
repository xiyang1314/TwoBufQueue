#include "../support/read_test_support.hpp"
using namespace read_test;
struct Item {
    unsigned value = 0;
    bool reset(std::uint64_t) noexcept { value = 0; return true; }
};
template<class Queue> static void start_record(Queue& queue) {
    auto start = queue.try_pop();
    CHECK(start && !start.get());
    queue.finish(start);
}
static void roles() {
    WarmPool<Item> pool(1,4); SpscLiveHandoffQueue queue(pool,4);
    auto a = queue.try_acquire(); CHECK(a);
    CHECK(queue.current_producer().get() == a.loan.get());
    CHECK(queue.try_acquire().status == LiveAcquireStatus::ProducerBusy);
    std::size_t count = 0;
    CHECK(queue.snapshot_for_read(count).get() == a.loan.get() && count == 1);
    auto start = queue.try_pop();
    CHECK(start && !start.get() && start.next().get() == a.loan.get() && queue.empty());
    CHECK(queue.is_pending(start)); queue.finish(start); CHECK(!start);
    a.loan->value = 1;
    auto b = queue.send_and_acquire(); CHECK(b); b.loan->value = 2;
    CHECK(queue.snapshot_for_read(count).get() == b.loan.get() && count == 1);
    auto first = queue.try_pop(); CHECK(first && first->value == 1 && first.next().get() == b.loan.get());
    CHECK(queue.empty() && pool.in_use_count() == 2 && !queue.try_pop());
    CHECK(queue.snapshot_for_read(count).get() == b.loan.get() && count == 0);
    CHECK(queue.send()); auto second = queue.try_pop(); CHECK(second->value == 2 && !second.next());
    queue.close(); CHECK(queue.empty() && !queue.is_drained());
    auto stale = first; queue.finish(first);
    CHECK(!first && !queue.is_pending(stale) && queue.is_pending(second) && pool.in_use_count() == 1);
    queue.finish(second); CHECK(queue.is_drained() && pool.all_released());
}
static void capacity() {
    WarmPool<Item> pool(0,100); SpscLiveHandoffQueue queue(pool,2,{},true);
    CHECK(queue.try_acquire());
    auto next = queue.send_and_acquire(); CHECK(!next && next.status == LiveAcquireStatus::QueueFull);
    CHECK(queue.pending_count() == 2 && pool.in_use_count() == 1);
    start_record(queue); auto claim = queue.try_pop(); CHECK(claim.get() && queue.empty());
    CHECK(queue.stats().queue_full == 1 && queue.stats().pool_limit == 0);
    // The popped record still owns its resource, but its queue slot is already reusable.
    next = queue.try_acquire(); CHECK(next && pool.in_use_count() == 2);
    queue.finish(claim); queue.close();
    CHECK(queue.try_acquire().status == LiveAcquireStatus::ProducerBusy && queue.send());
    start_record(queue); claim = queue.try_pop(); CHECK(claim.get()); queue.finish(claim);
    CHECK(queue.try_acquire().status == LiveAcquireStatus::Closed && queue.is_drained());
}
static void current_snapshot() {
    WarmPool<Item> pool(4,4); SpscLiveHandoffQueue queue(pool,8);
    CHECK(!queue.current());
    auto a = queue.try_acquire(); CHECK(a); a.loan->value = 1;
    start_record(queue);
    CHECK(queue.current().get() == a.loan.get());
    auto b = queue.send_and_acquire(); CHECK(b); b.loan->value = 2;
    auto c = queue.send_and_acquire(); CHECK(c); c.loan->value = 3;
    std::size_t count = 0;
    const auto current = queue.snapshot_for_read(count);
    CHECK(count == 2 && current.get() == c.loan.get()); // tail next, not the saved current A
    CHECK(queue.current().get() == a.loan.get());

    // A later handoff must not change this round's boundary or candidate current.
    auto d = queue.send_and_acquire(); CHECK(d); d.loan->value = 4;
    for (unsigned expected = 1; expected <= count; ++expected) {
        auto record = queue.try_pop(); CHECK(record && record->value == expected);
        queue.finish(record);
    }
    CHECK(queue.pending_count() == 1 && current->value == 3 && queue.current().get() == current.get());
    auto pending = queue.try_pop(); CHECK(pending->value == 3);
    const auto empty_current = queue.snapshot_for_read(count);
    CHECK(count == 0 && empty_current.get() == d.loan.get() && queue.current().get() == d.loan.get());
    queue.finish(pending); // pending must finish before consuming empty_current
    CHECK(empty_current->value == 4);
    CHECK(queue.send()); queue.close();
    pending = queue.try_pop(); queue.finish(pending);
    CHECK(!queue.current() && queue.is_drained() && pool.all_released());
}
static void single_pool() {
    WarmPool<Item> pool(1,1); SpscLiveHandoffQueue queue(pool,8,{},true);
    CHECK(queue.try_acquire()); start_record(queue);
    const auto b = queue.send_and_acquire(); CHECK(!b && b.status == LiveAcquireStatus::PoolLimit);
    auto claim = queue.try_pop(); CHECK(claim.get() && queue.empty());
    CHECK(queue.try_acquire().status == LiveAcquireStatus::PoolLimit); // pop does not recycle
    queue.finish(claim);
    CHECK(queue.try_acquire() && queue.send());
    start_record(queue); claim = queue.try_pop(); CHECK(claim.get()); queue.finish(claim);
    queue.close(); CHECK(queue.is_drained() && pool.all_released() && queue.stats().pool_limit == 2);
}
static void fifo_concurrent() {
    WarmPool<Item> pool(8,8); SpscLiveHandoffQueue queue(pool,8);
    constexpr unsigned count = 50'000;
    std::thread producer([&] {
        for (unsigned i = 0; i != count; ++i) {
            auto acquired = queue.try_acquire();
            CHECK(wait_until([&] { if (!acquired) acquired = queue.try_acquire(); return bool(acquired); }));
            acquired.loan->value = i; CHECK(queue.send());
        }
        queue.close();
    });
    unsigned next = 0;
    decltype(queue)::Record pending;
    bool delayed = false;
    CHECK(wait_until([&] {
        if (!pending) pending = queue.try_pop();
        if (pending) {
            if (pending.get()) {
                CHECK(pending->value == next);
                if (next % 7 == 0 && !delayed) { delayed = true; return false; }
                delayed = false;
                ++next;
            }
            queue.finish(pending);
        }
        return next == count;
    }, 20s));
    producer.join(); CHECK(queue.is_drained() && pool.all_released());
}
struct Fallible { bool* reset_ok; bool reset(std::uint64_t) noexcept { return *reset_ok; } };
struct Factory {
    bool* create_ok; bool* reset_ok;
    Fallible* create() { return *create_ok ? new Fallible{reset_ok} : nullptr; }
    void dispose(Fallible* value) noexcept { delete value; }
};
static void resource_failures() {
    bool create_ok = false, reset_ok = false;
    WarmPool<Fallible,true,Factory> pool(0,2,{&create_ok,&reset_ok});
    SpscLiveHandoffQueue queue(pool,2,{},true);
    CHECK(queue.try_acquire().status == LiveAcquireStatus::ResourceError); create_ok = true;
    CHECK(queue.try_acquire().status == LiveAcquireStatus::ResourceError && queue.empty() && pool.all_released());
    reset_ok = true; CHECK(queue.try_acquire() && queue.send());
    start_record(queue); auto claim = queue.try_pop(); CHECK(claim.get()); queue.finish(claim);
    queue.close(); CHECK(queue.is_drained() && queue.stats().resource_error == 2 && pool.all_released());
}
struct PublishedItem {
    std::atomic<unsigned> published{0};
    Gate* gate = nullptr;
    bool reset(std::uint64_t) noexcept { published = 0; gate = nullptr; return true; }
};
static void finish_publish_window() {
    WarmPool<PublishedItem> pool(1,1);
    SpscLiveHandoffQueue queue(pool,2,{[](PublishedItem& item) noexcept { item.gate->pause(); }});
    auto producer = queue.try_acquire(); auto start = queue.try_pop();
    auto current = start.next(); queue.finish(start);
    Gate gate; producer.loan->gate = &gate;
    std::thread writer([&] { producer.loan->published = 42; CHECK(queue.send()); queue.close(); });
    gate.wait();
    CHECK(!queue.try_pop());
    CHECK(queue.current().get() == current.get()); // no completed record yet: use saved current
    std::size_t count = 0; CHECK(queue.snapshot_for_read(count).get() == current.get() && count == 0);
    CHECK(current->published == 42 && pool.in_use_count() == 1);
    gate.release(); writer.join();
    auto claim = queue.try_pop(); CHECK(claim.get() == current.get() && !claim.next()); queue.finish(claim);
    CHECK(queue.is_drained() && pool.all_released());
}
int main(int argc, char** argv) {
    return run_cases(argc,argv,{{"roles",roles},{"capacity",capacity},{"current_snapshot",current_snapshot},{"single_pool",single_pool},
        {"fifo_concurrent",fifo_concurrent},{"resource_failures",resource_failures},{"finish_publish_window",finish_publish_window}});
}
