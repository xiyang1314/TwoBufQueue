#include <TwoBufQueue.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace TwoBufQueue;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Predicate>
static bool until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}
// 用于失败构造的生命周期清理；不执行用户回调。
template<class WriterT>
static void finish_and_drain(WriterT& writer) {
    writer.finish();
    auto& source = writer.handoff_queue();
    source.close();
    while (auto record = source.try_pop()) source.finish(record);
}

static void plain_records(bool list_constructor) {
    const auto thread = std::this_thread::get_id();
    const std::uint32_t a = 0x12345678;
    const std::uint16_t b = 0xabcd;
    const std::uint8_t c = 0xef;
    std::array<std::byte, sizeof(a) + sizeof(b) + sizeof(c)> packed;
    std::memcpy(packed.data(), &a, sizeof(a));
    std::memcpy(packed.data() + sizeof(a), &b, sizeof(b));
    std::memcpy(packed.data() + sizeof(a) + sizeof(b), &c, sizeof(c));
    std::array<std::byte, 80> raw{}; // 大于源块，验证同步消息不占用源块空间。
    raw.back() = std::byte{42};
    unsigned received = 0;
    bool valid = true;
    auto callback = [&](std::int32_t id, const std::byte* data, std::size_t size) noexcept -> void {
        valid &= id == 7 && std::this_thread::get_id() == thread;
        switch (received++) {
        case 0:
            valid &= size == sizeof(a) && data == reinterpret_cast<const std::byte*>(&a);
            if (size == sizeof(a)) valid &= std::memcmp(data, &a, size) == 0;
            break;
        case 1:
            valid &= size == packed.size();
            if (size == packed.size()) valid &= std::memcmp(data, packed.data(), size) == 0;
            break;
        case 2: case 3: valid &= size == 0; break;
        case 4:
            valid &= size == raw.size() && data == raw.data();
            if (size == raw.size()) valid &= std::memcmp(data, raw.data(), size) == 0;
            break;
        default: valid = false;
        }
    };
    using Context = SyncContext<decltype(callback)>;
    using MyWriter = Writer<1, true, std::uint16_t, Context>;
    static_assert(MyWriter::is_sync && !Writer<>::is_sync);
    static_assert(std::is_void_v<Writer<>::SyncContextType>);
    auto writer = make_writer<MyWriter>({.block_size = 16, .pool_min = 1, .pool_max = 1, .thread_id = 7});
    bool ok = !writer.write(a); // 未绑定时拒绝，不进入异步路径。
    using Queue = ::TwoBufQueue::TwoBufQueue<decltype(callback), decltype(writer)>;
    std::unique_ptr<Queue> queue;
    if (list_constructor) queue = std::make_unique<Queue>(callback, std::initializer_list<decltype(writer)*>{&writer});
    else queue = std::make_unique<Queue>(callback, writer);
    auto* source = writer.source_block();
    auto* const initial = writer.published_write_ptr();
    ok &= queue->started() && !writer.handoff_queue().has_read_available_callback();
    ok &= writer.write(a) && received == 1;
    ok &= writer.write(a, b, c) && received == 2;
    ok &= writer.write() && writer.write_mem(nullptr, 0) && received == 4;
    ok &= !writer.write_mem(nullptr, 1) && received == 4;
    ok &= writer.write_mem(raw.data(), static_cast<std::uint16_t>(raw.size())) && received == 5;
    writer.flush();
    ok &= writer.source_block() == source && writer.published_write_ptr() == initial && source->readable().empty();
    ok &= writer.handoff_queue().pending_count() == 1 && writer.backpressure_stats().pressure_events == 0;
    writer.finish();
    writer.finish(); // 幂等；不重复执行业务回调。
    ok &= !writer.write(a) && !writer.write_mem(raw.data(), 1) && received == 5;
    queue->stop(); queue->stop();
    ok &= !queue->started() && !writer.has_sync_callback() && writer.handoff_queue().is_drained();
    ok &= writer.handoff_queue().pool().in_use_count() == 0 && queue->stats().saved_data_bytes == 0;
    ok &= queue->error_code() == ReadError::none;
    require(ok && valid, "plain synchronous callback, payload layout or source lifecycle failed");
}

template<class Seq>
static void seq_records() {
    const auto thread = std::this_thread::get_id();
    constexpr Seq sequence = (std::numeric_limits<Seq>::max)();
    const std::uint16_t value = 123;
    const std::uint8_t tail = 4;
    std::array<std::byte, 3> packed;
    std::memcpy(packed.data(), &value, sizeof(value));
    std::memcpy(packed.data() + sizeof(value), &tail, sizeof(tail));
    unsigned received = 0;
    bool valid = true;
    auto callback = [&](std::int32_t id, Seq seq, const std::byte* data, std::size_t size) noexcept -> void {
        valid &= id == 2 && seq == sequence && std::this_thread::get_id() == thread;
        const auto index = received++;
        if (index == 0 || index == 1) {
            valid &= size == packed.size();
            if (size == packed.size()) valid &= std::memcmp(data, packed.data(), size) == 0;
        } else valid &= index == 2 && size == 0;
    };
    auto writer = make_writer<SeqWriter<Seq, 1, true, std::uint16_t, SyncContext<decltype(callback)>>>({.thread_id = 2});
    ::TwoBufQueue::TwoBufQueue queue(callback, writer);
    const auto initial = writer.published_write_ptr();
    bool ok = writer.write(sequence, value, tail) && received == 1;
    ok &= writer.write_mem(sequence, packed.data(), static_cast<std::uint16_t>(packed.size())) && received == 2;
    ok &= writer.write(sequence) && received == 3;
    ok &= !writer.write_mem(sequence, nullptr, 1) && received == 3;
    writer.flush();
    ok &= writer.published_write_ptr() == initial;
    writer.finish();
    ok &= !writer.write(sequence, value) && !writer.write_mem(sequence, packed.data(), 1);
    queue.stop();
    require(ok && valid && received == 3 && writer.handoff_queue().is_drained(), "SeqWriter callback signature or seq/payload separation failed");
}

static void callback_blocks_producer() {
    std::atomic<bool> entered{false}, released{false}, returned{false}, valid{true};
    std::thread::id writer_thread;
    auto callback = [&](std::int32_t id, const std::byte*, std::size_t size) noexcept -> void {
        valid.store(id == 0 && size == sizeof(std::uint32_t) && std::this_thread::get_id() == writer_thread);
        entered.store(true, std::memory_order_release);
        while (!released.load(std::memory_order_acquire)) std::this_thread::yield();
    };
    auto writer = make_writer<Writer<1, true, std::uint16_t, SyncContext<decltype(callback)>>>();
    ::TwoBufQueue::TwoBufQueue queue(callback, writer);
    std::thread producer([&] {
        writer_thread = std::this_thread::get_id();
        const bool written = writer.write(std::uint32_t{10});
        if (!written) valid.store(false);
        returned.store(true, std::memory_order_release);
        writer.finish();
    });
    const bool began = until([&] { return entered.load(std::memory_order_acquire); });
    const bool blocked = !returned.load(std::memory_order_acquire);
    released.store(true, std::memory_order_release);
    producer.join(); queue.stop();
    require(began && blocked && returned.load() && valid.load(), "write returned before producer-thread callback completed");
}

// 同一 Context 类型在单 Writer 构造（引用或单元素列表）时不加锁。
// 递归处理第二条消息能返回，验证未错误地取得非递归互斥锁。
struct NestedCallback {
    using Context = SyncContext<NestedCallback>;
    using MyWriter = Writer<1, true, std::uint16_t, Context>;
    MyWriter* writer;
    unsigned& received;
    bool& valid;
    void operator()(std::int32_t id, const std::byte* data, std::size_t size) noexcept {
        std::uint32_t value = 0;
        if (size == sizeof(value)) std::memcpy(&value, data, size);
        valid &= id == 0 && size == sizeof(value) && value == ++received;
        if (value == 1) valid &= writer->write(std::uint32_t{2});
    }
};

static void single_writer_without_lock(bool list_constructor) {
    unsigned received = 0;
    bool valid = true;
    auto writer = make_writer<NestedCallback::MyWriter>();
    NestedCallback callback{&writer, received, valid};
    using Queue = ::TwoBufQueue::TwoBufQueue<NestedCallback, decltype(writer)>;
    std::unique_ptr<Queue> queue;
    if (list_constructor) queue = std::make_unique<Queue>(callback, std::initializer_list<decltype(writer)*>{&writer});
    else queue = std::make_unique<Queue>(callback, writer);
    const bool written = writer.write(std::uint32_t{1});
    writer.finish(); queue->stop();
    require(written && valid && received == 2 && writer.handoff_queue().is_drained(),
            "single Writer unexpectedly locked its callback or failed nested delivery");
}

template<bool WithSeq>
static void multi_writer_records() {
    constexpr unsigned writer_count = 4, per_writer = 1000;
    std::array<unsigned, writer_count> received{};
    std::array<std::thread::id, writer_count> producer_threads{};
    std::atomic<unsigned> active{0};
    std::atomic<bool> valid{true};
    auto check = [&](std::int32_t id, std::uint32_t seq, const std::byte* data, std::size_t size) noexcept {
        if (active.fetch_add(1, std::memory_order_acq_rel) != 0) valid.store(false);
        if (id < 0 || id >= static_cast<std::int32_t>(writer_count) || size != sizeof(std::uint32_t)) valid.store(false);
        else {
            std::uint32_t value = 0;
            std::memcpy(&value, data, sizeof(value));
            const auto index = static_cast<unsigned>(id);
            if (value != received[index] || (WithSeq && seq != value) ||
                std::this_thread::get_id() != producer_threads[index]) valid.store(false);
            ++received[index]; // 普通非原子数据，依靠队列的共享锁串行访问。
        }
        std::this_thread::yield();
        active.fetch_sub(1, std::memory_order_release);
    };
    auto callback = [&] {
        if constexpr (WithSeq) {
            return [&](std::int32_t id, std::uint32_t seq, const std::byte* data, std::size_t size) noexcept -> void {
                check(id, seq, data, size);
            };
        } else {
            return [&](std::int32_t id, const std::byte* data, std::size_t size) noexcept -> void {
                check(id, 0, data, size);
            };
        }
    }();
    using Context = SyncContext<decltype(callback)>;
    using MyWriter = std::conditional_t<WithSeq, SeqWriter<std::uint32_t, 1, true, std::uint16_t, Context>,
                                      Writer<1, true, std::uint16_t, Context>>;
    auto w0 = make_writer<MyWriter>({.block_size = 16, .pool_min = 1, .pool_max = 1, .thread_id = 0});
    auto w1 = make_writer<MyWriter>({.block_size = 16, .pool_min = 1, .pool_max = 1, .thread_id = 1});
    auto w2 = make_writer<MyWriter>({.block_size = 16, .pool_min = 1, .pool_max = 1, .thread_id = 2});
    auto w3 = make_writer<MyWriter>({.block_size = 16, .pool_min = 1, .pool_max = 1, .thread_id = 3});
    ::TwoBufQueue::TwoBufQueue queue(callback, {&w0, &w1, &w2, &w3});
    std::array<OwnedWriter<MyWriter>*, writer_count> writers{&w0, &w1, &w2, &w3};
    {
        std::array<std::thread, writer_count> threads;
        for (unsigned i = 0; i < writer_count; ++i) threads[i] = std::thread([&, i] {
            producer_threads[i] = std::this_thread::get_id();
            for (std::uint32_t value = 0; value < per_writer; ++value) {
                bool written;
                if constexpr (WithSeq) {
                    if (value % 2 == 0) written = writers[i]->write(value, value);
                    else written = writers[i]->write_mem(value, &value, sizeof(value));
                } else {
                    if (value % 2 == 0) written = writers[i]->write(value);
                    else written = writers[i]->write_mem(&value, sizeof(value));
                }
                if (!written) valid.store(false);
            }
            writers[i]->finish();
        });
        for (auto& thread : threads) thread.join();
    }
    queue.stop();
    for (unsigned i = 0; i < writer_count; ++i)
        require(received[i] == per_writer && writers[i]->handoff_queue().is_drained() &&
                writers[i]->handoff_queue().pool().in_use_count() == 0, "multi-writer count, ordering or drain failed");
    require(valid.load() && active.load() == 0, "multi-writer callbacks overlapped or ran on another thread");
}

static void registration_checks() {
    auto callback = [](std::int32_t, const std::byte*, std::size_t) noexcept -> void {};
    auto w0 = make_writer<Writer<1, true, std::uint16_t, SyncContext<decltype(callback)>>>({.thread_id = 0});
    auto w1 = make_writer<Writer<1, true, std::uint16_t, SyncContext<decltype(callback)>>>({.thread_id = 1});
    bool bound = false;
    {
        ::TwoBufQueue::TwoBufQueue queue(callback, {&w0, &w1});
        bound = w0.has_sync_callback() && w1.has_sync_callback();
        w0.finish(); w1.finish();
        queue.stop();
    }
    require(bound && !w0.has_sync_callback() && !w1.has_sync_callback(),
            "same sync Writer type failed to bind multiple writers");

    auto m0 = make_writer<Writer<1, true, std::uint16_t, SyncContext<decltype(callback)>>>({.thread_id = 0});
    auto m1 = make_writer<Writer<1, true, std::uint16_t, SyncContext<decltype(callback)>>>({.thread_id = 0});
    bool rejected = false;
    try { ::TwoBufQueue::TwoBufQueue queue(callback, {&m0, &m1}); }
    catch (const std::invalid_argument&) { rejected = true; }
    const bool duplicate_unbound = !m0.has_sync_callback() && !m1.has_sync_callback();
    finish_and_drain(m0); finish_and_drain(m1);
    require(rejected && duplicate_unbound, "duplicate thread ids accepted or partially bound on failure");

    auto async = make_writer<Writer<>>();
    rejected = false;
    try { ::TwoBufQueue::TwoBufQueue queue(callback, async, {.output_mode = HandoffMode::Sync}); }
    catch (const std::invalid_argument&) { rejected = true; }
    finish_and_drain(async);
    require(rejected, "ordinary Writer silently used copy-thread sync mode");
}

int main() try {
    plain_records(false); plain_records(true);
    seq_records<std::uint8_t>(); seq_records<std::uint32_t>(); seq_records<std::uint64_t>();
    callback_blocks_producer();
    single_writer_without_lock(false); single_writer_without_lock(true);
    multi_writer_records<false>(); multi_writer_records<true>();
    registration_checks();
    std::cout << "PASS: producer-thread sync callbacks, packed payloads, shared lock and lifecycle\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
