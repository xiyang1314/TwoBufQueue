#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
static void pause_index(int point);
static void cancel_phase(int point);
#define BRQ_INDEX_QUEUE_TEST_HOOK(point) pause_index(point)
#define BRQ_READY_QUEUE_TEST_HOOK(point) cancel_phase(point)
#include "TwoBufQueue/src/base_components/HandoffQueue/handoff_queue.hpp"
#include "TwoBufQueue/src/two_buf_queue.hpp"
#undef BRQ_INDEX_QUEUE_TEST_HOOK
#undef BRQ_READY_QUEUE_TEST_HOOK

using namespace TwoBufQueue;
using namespace std::chrono_literals;
static thread_local int pause_at = 0;
static std::atomic<bool> paused{false}, resume{false}, publishing_cancel{false};
template<class F> static void until(F condition) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
static void pause_index(int point) {
    if (pause_at != point) return;
    pause_at = 0;
    paused.store(true, std::memory_order_release);
    until([] { return resume.load(std::memory_order_acquire); });
}
static void cancel_phase(int) { publishing_cancel.store(true, std::memory_order_release); }
struct Item {
    int value = 0;
    bool reset(std::uint64_t) noexcept { value = 0; return true; }
};
using Pool = WarmPool<Item>;
using Queue = PooledResourceQueue<Pool>;
static void send(Queue& queue, int value) {
    auto item = queue.try_acquire();
    assert(item);
    item.loan->value = value;
    assert(queue.send(item.loan));
}
static void expect(Queue& queue, int value) {
    auto claim = queue.try_claim();
    assert(claim && claim->value == value);
    assert(queue.finish(claim));
}
static void drained(Queue& queue, Pool& pool) {
    queue.close();
    assert(!queue.has_ready() && queue.is_drained() && pool.all_released());
}

static void order() {
    for (auto mode : {CancelClaimMode::Keep, CancelClaimMode::QueueTail}) {
        Pool pool(4, 4);
        Queue queue(pool, 4, true, mode);
        // acquire 顺序与 send 顺序相反；必须按实际入队顺序，不按池槽位/资源地址。
        auto second = queue.try_acquire();
        auto first = queue.try_acquire();
        first.loan->value = 1; second.loan->value = 2;
        assert(queue.send(first.loan) && queue.send(second.loan));
        send(queue, 3);
        auto a = queue.try_claim();
        auto b = queue.try_claim();
        assert(a && a->value == 1 && b && b->value == 2);
        assert(queue.cancel_claim(b) && queue.cancel_claim(a)); // 逆序取消。
        if (mode == CancelClaimMode::Keep) {
            expect(queue, 1); expect(queue, 2); expect(queue, 3);
        } else {
            expect(queue, 3); expect(queue, 2); expect(queue, 1);
        }
        drained(queue, pool);
    }
}

static void receive_and_reuse() {
    Pool pool(3, 3);
    Queue queue(pool, 3, true, CancelClaimMode::Keep);
    send(queue, 1); send(queue, 2); send(queue, 3);
    auto a = queue.try_claim();
    auto b = queue.try_claim();
    assert(a->value == 1 && b->value == 2);
    assert(queue.cancel_claim(a));
    auto received = queue.try_receive(); // 普通 receive 也必须先领取取消的 a。
    assert(received && received->value == 1);
    assert(queue.finish(received));
    send(queue, 4);                     // 复用刚释放的槽位，仍排在 3 后面。
    expect(queue, 3); expect(queue, 4);
    assert(queue.cancel_claim(b));
    expect(queue, 2);
    for (int i = 5; i < 1005; ++i) {
        send(queue, i);
        auto claim = queue.try_claim();
        assert(queue.cancel_claim(claim));
        expect(queue, i);
    }
    drained(queue, pool);
}

static void stale() {
    Pool pool(1, 1);
    Queue queue(pool, 1, true, CancelClaimMode::Keep);
    send(queue, 1);
    auto claim = queue.try_claim();
    auto old = claim;
    std::atomic<int> cancelled{0};
    std::thread a([&, copy = claim]() mutable { cancelled += queue.cancel_claim(copy); });
    std::thread b([&, copy = claim]() mutable { cancelled += queue.cancel_claim(copy); });
    a.join(); b.join();
    assert(cancelled == 1 && queue.stats().ready_count == 1);
    claim = queue.try_claim();
    assert(claim && !queue.cancel_claim(old) && !queue.finish(old));
    assert(!queue.try_claim());
    assert(queue.finish(claim));
    drained(queue, pool);
}

static void parallel() {
    Pool pool(4, 4);
    Queue queue(pool, 4, false, CancelClaimMode::Keep);
    send(queue, 1); send(queue, 2); send(queue, 3);
    std::atomic<bool> held{false}, release{false};
    std::thread first([&] {
        auto claim = queue.try_claim();
        assert(claim && claim->value == 1);
        held.store(true, std::memory_order_release);
        until([&] { return release.load(std::memory_order_acquire); });
        assert(queue.cancel_claim(claim));
    });
    until([&] { return held.load(std::memory_order_acquire); });
    expect(queue, 2); expect(queue, 3); // a 正在使用不阻塞 b、c。
    for (int i = 4; i < 1004; ++i) { send(queue, i); expect(queue, i); }
    release.store(true, std::memory_order_release);
    first.join();
    send(queue, 1004);
    expect(queue, 1); expect(queue, 1004);
    drained(queue, pool);
}

static void first_order() {
    ReadyIndexQueue queue(3, CancelClaimMode::Keep);
    assert(queue.try_push(0) && queue.try_push(1) && queue.try_push(2));
    paused = false; resume = false;
    std::uint32_t a = 99, b = 99, value = 99;
    std::thread first([&] { pause_at = 3; assert(queue.try_pop(a)); }); // CAS 成功后暂停。
    until([] { return paused.load(std::memory_order_acquire); });
    assert(queue.try_pop(b) && b == 1); // 第二个出队先返回。
    resume.store(true, std::memory_order_release);
    first.join();
    assert(a == 0);
    assert(queue.requeue(b) && queue.requeue(a));
    for (std::uint32_t expected : {0u, 1u, 2u}) {
        assert(queue.try_pop(value) && value == expected);
    }
    assert(!queue.has_ready());
}

static void cancel_race() {
    ReadyIndexQueue queue(3, CancelClaimMode::Keep);
    std::uint32_t a, b = 99, value;
    assert(queue.try_push(0) && queue.try_pop(a));
    assert(queue.try_push(1));
    paused = false; resume = false; publishing_cancel = false;
    std::thread normal([&] { pause_at = 4; assert(queue.try_pop(b)); }); // FIFO CAS 前暂停。
    until([] { return paused.load(std::memory_order_acquire); });
    std::atomic<bool> cancelled{false};
    std::thread cancel([&] { assert(queue.requeue(a)); cancelled.store(true); });
    until([] { return publishing_cancel.load(std::memory_order_acquire); });
    assert(!cancelled.load());
    assert(queue.try_push(2)); // 新发送仍可前进，但新领取不能越过正在发布的取消。
    assert(!queue.try_pop(value));
    resume.store(true, std::memory_order_release);
    normal.join(); cancel.join();
    assert(b == 1);
    assert(queue.try_pop(value) && value == 0);
    assert(queue.try_pop(value) && value == 2);
    assert(!queue.has_ready());
}

static void sync() {
    Pool pool(3, 3);
    HandoffOptions options;
    options.mode = HandoffMode::Sync;
    options.queue_capacity = 3;
    options.cancel_claim_mode = CancelClaimMode::Keep;
    bool accept = false;
    std::vector<int> calls;
    HandoffQueue queue(pool, options, [&](Item& item) noexcept {
        calls.push_back(item.value);
        return accept;
    });
    for (int i = 1; i <= 3; ++i) {
        auto item = queue.try_acquire();
        item.loan->value = i;
        assert(queue.send(item.loan));
    }
    assert((calls == std::vector<int>{1, 1, 1}));
    accept = true;
    queue.retry_pending();
    assert((calls == std::vector<int>{1, 1, 1, 1, 2, 3}));
    queue.close();
    assert(queue.is_drained() && pool.all_released());
}

static void configuration() {
    assert(HandoffOptions{}.cancel_claim_mode == CancelClaimMode::QueueTail);
    const auto bad = static_cast<CancelClaimMode>(99);
    bool rejected = false;
    try { ClaimQueue<Item> queue(2, false, bad); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    Pool pool(1, 1);
    HandoffOptions options;
    options.cancel_claim_mode = bad;
    rejected = false;
    try { HandoffQueue queue(pool, options, [](Item&) noexcept { return true; }); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected && pool.all_released());
}

static void two_buf() {
    WriterSourceConfig source;
    source.block_size = 64; source.pool_min = 4; source.pool_max = 8;
    auto writer = make_writer<Writer<1, true, std::uint32_t>>(source);
    TwoBufQueueOptions options;
    options.output_block_size = 64; options.output_frame_capacity = 1;
    options.output_pool_min = 4; options.output_pool_max = 4;
    options.cancel_claim_mode = CancelClaimMode::Keep;
    std::atomic<bool> accept{false}, out_of_order{false};
    std::atomic<int> attempts{0};
    std::vector<int> completed;
    auto callback = [&](const DataBatch& batch) noexcept {
        assert(batch.frames.size() == 1 && batch.frames[0].size == 64);
        const int value = std::to_integer<int>(batch.frames[0].data[sizeof(std::uint32_t)]);
        if (!accept.load(std::memory_order_acquire)) {
            if (value != 1) out_of_order.store(true);
            attempts.fetch_add(1, std::memory_order_release);
            return false;
        }
        completed.push_back(value);
        return true;
    };
    TwoBufQueueBatch queue(callback, writer, options);
    assert(queue.started());
    for (unsigned char i = 1; i <= 3; ++i) {
        unsigned char data[60]{};
        data[0] = i;
        assert(writer.write_mem(data, sizeof(data)));
    }
    writer.finish();
    until([&] { return attempts.load(std::memory_order_acquire) >= 3; });
    accept.store(true, std::memory_order_release);
    queue.stop();
    assert(!out_of_order.load() && (completed == std::vector<int>{1, 2, 3}));
    assert(queue.error_code() == ReadError::none);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string name = argv[1];
    if (name == "order") order();
    else if (name == "receive") receive_and_reuse();
    else if (name == "stale") stale();
    else if (name == "parallel") parallel();
    else if (name == "first_order") first_order();
    else if (name == "race") cancel_race();
    else if (name == "sync") sync();
    else if (name == "config") configuration();
    else if (name == "two_buf") two_buf();
    else assert(false);
    std::cout << "PASS cancel_modes " << name << '\n';
}
