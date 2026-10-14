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
using namespace std::chrono_literals;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class TPredicate>
static bool until(TPredicate predicate, std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}
template<class TWriter> struct ReleaseOnExit {
    std::atomic<bool>& released;
    TWriter& writer;
    ~ReleaseOnExit() { released.store(true,std::memory_order_release); writer.finish(); }
};

template<class TWriter> ReleaseOnExit(std::atomic<bool>&, TWriter&) -> ReleaseOnExit<TWriter>;

// Suspend the first callback; only separate copy/callback threads allow two source blocks to be recycled repeatedly for the full send round.
static void copied_while_callback_waits(bool list_constructor, HandoffMode mode, ReaderMode reader_mode) {
    WriterSourceConfig source;
    source.block_size = 64; source.pool_min = source.pool_max = 2;
    auto writer = make_writer<Writer<>>(source);
    TwoBufQueueOptions options;
    options.output_block_size = 64; options.output_frame_capacity = 1;
    // With frame_capacity = 1, each record may occupy an output block. Hold all 100 records while the callback blocks;
    // never assume the copy thread always fills a source block in one read, or scheduling could look like callbacks blocked copying.
    options.output_pool_min = options.output_pool_max = 128;
    options.output_queue_capacity = 128;
    options.output_mode = mode; options.output_reader_mode = reader_mode;
    options.output_enable_stats = true;
    std::atomic<bool> entered{false}, released{false};
    std::uint64_t received = 0;
    bool valid = true;
    auto callback = [&](std::int32_t id, const std::byte* data, std::size_t size) noexcept {
        std::uint64_t sequence = 0;
        if (size == sizeof(sequence)) std::memcpy(&sequence,data,sizeof(sequence));
        valid &= id == 0 && size == sizeof(sequence) && sequence == received;
        if (received == 0) {
            entered.store(true,std::memory_order_release);
            while (!released.load(std::memory_order_acquire)) std::this_thread::yield();
        }
        ++received;
    };
    using Queue = ::TwoBufQueue::TwoBufQueue<decltype(callback),decltype(writer)>;
    std::unique_ptr<Queue> queue;
    if (list_constructor) queue = std::make_unique<Queue>(callback,std::initializer_list<decltype(writer)*>{&writer},options);
    else queue = std::make_unique<Queue>(callback,writer,options);
    ReleaseOnExit release{released,writer}; // On exception, release the callback wait and finish production before destroying the queue.
    require(queue->started(),"copy worker did not start");
    std::uint64_t sent = 0;
    require(writer.write(sent),"initial write failed"); ++sent; writer.flush();
    require(until([&] { return entered.load(std::memory_order_acquire); }),"callback did not start");
    constexpr std::uint64_t total = 100;
    const bool copied = until([&] {
        if (sent == total) return true;
        if (writer.write(sent)) ++sent;
        return sent == total;
    }, 2000ms);
    released.store(true,std::memory_order_release);
    const bool remaining = until([&] {
        if (sent == total) return true;
        if (writer.write(sent)) ++sent;
        return sent == total;
    });
    writer.finish(); queue->stop();
    require(remaining && valid && received == total && queue->error_code() == ReadError::none,
            "FIFO, count or drain failed");
    require(copied,"asynchronous callback blocked copying");
}

// Two output workers enter callbacks concurrently, verifying output_reader_count is forwarded correctly.
static void parallel_batch_callbacks() {
    WriterSourceConfig source;
    source.block_size = 64; source.pool_min = source.pool_max = 4;
    auto writer = make_writer<Writer<1,false>>(source);
    TwoBufQueueOptions options;
    options.output_block_size = 64; options.output_frame_capacity = 1;
    options.output_pool_min = options.output_pool_max = 4;
    options.output_mode = HandoffMode::SPMC;
    options.output_reader_count = 2;
    std::atomic<unsigned> entered{0}, completed{0};
    std::atomic<bool> released{false}, valid{true};
    std::array<std::thread::id,2> threads;
    auto callback = [&](const DataBatch& batch) noexcept {
        const auto index = entered.fetch_add(1,std::memory_order_acq_rel);
        if (index < threads.size()) threads[index] = std::this_thread::get_id();
        else valid.store(false);
        if (batch.frames.size() != 1 || batch.data_size != 64) valid.store(false);
        while (!released.load(std::memory_order_acquire)) std::this_thread::yield();
        completed.fetch_add(1,std::memory_order_release);
        return true;
    };
    TwoBufQueueBatch queue(callback,writer,options);
    ReleaseOnExit release{released,writer};
    std::array<std::byte,64> payload{};
    const bool written = writer.write_mem(payload.data(),payload.size()) && writer.write_mem(payload.data(),payload.size());
    writer.flush();
    const bool parallel = until([&] { return entered.load(std::memory_order_acquire) == 2; });
    released.store(true,std::memory_order_release);
    writer.finish(); queue.stop();
    require(written && parallel && valid.load() && completed.load() == 2 && threads[0] != threads[1],
            "output callback worker count was not forwarded");
    require(queue.error_code() == ReadError::none,"batch pipeline error");
}

int main() try {
    require(TwoBufQueueOptions{}.output_mode == HandoffMode::MPSC,"default output must be asynchronous");
    for (bool list : {false,true}) {
        copied_while_callback_waits(list,HandoffMode::MPSC,ReaderMode::WaitSignal);
        copied_while_callback_waits(list,HandoffMode::SPSC,ReaderMode::Polling);
    }
    parallel_batch_callbacks();
    std::cout << "5 TwoBufQueue output options scenarios passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
