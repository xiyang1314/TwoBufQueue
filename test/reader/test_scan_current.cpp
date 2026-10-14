#include "../support/read_test_support.hpp"
#include <TwoBufQueue.hpp>
#include "TwoBufQueue/src/write/seq_writer.hpp"
#include <array>
#include <limits>

using namespace read_test;
constexpr auto block_only = (std::numeric_limits<std::uint32_t>::max)();

// Drive Service directly; do not rely on scheduling or sleeps to prove current is not scanned.
template<bool Single, std::uint32_t Count> static void completed_only_service() {
    WriterFixture source(6, 4, 4);
    Writer<Count, true, std::uint16_t, void> writer(&source.queue, 0);
    Probe probe;
    AutoResetEvent event;
    std::atomic<bool> cancel{false};
    auto cfg = config();
    cfg.is_scan_current = false;
    auto exercise = [&](auto& service) {
        service.prepare_start();
        CHECK(!service.read_once(true, cancel).pending); // Consume the begin record.
        CHECK(writer.write(0u)); writer.flush();
        CHECK(writer.source_block()->readable().size() == 6);
        for (bool force : {false, true}) {
            CHECK(!service.read_once(force, cancel).pending);
            CHECK(probe.accepted == 0 && writer.source_block()->read_pos() == 0);
        }
        CHECK(writer.write(1u)); writer.flush(); // Record 0 is completed; record 1 remains current.
        probe.accept = false;
        CHECK(service.read_once(true, cancel).pending);
        CHECK(probe.accepted == 0 && probe.current_calls == 0);
        probe.accept = true;
        if constexpr (Single) CHECK(!service.read_once(true, cancel).pending);
        else {
            CHECK(!service.read_notified(cancel).pending); // Retain the notified source after rejection so it can retry.
            const auto calls = probe.completed_calls.load();
            CHECK(!service.read_notified(cancel).pending && probe.completed_calls == calls);
        }
        CHECK(probe.values(0) == std::vector<unsigned>{0});
        CHECK(writer.source_block()->read_pos() == 0);
        CHECK(service.next_scan_deadline() == cfg.idle_rescan_interval);
        writer.finish();
        CHECK(!service.read_once(true, cancel).pending);
        probe.verify(0, 2);
        CHECK(probe.current_calls == 0 && service.stats().saved_partial_frames == 0);
        CHECK(service.error_code() == ReadError::none && source.all_released());
        service.detach_completed_callbacks();
    };
    if constexpr (Single) {
        SingleReadService service(cfg, probe, event, writer);
        exercise(service);
    } else {
        ReadService service(cfg, probe, event);
        CHECK(service.add_writer(&writer));
        exercise(service);
    }
}

// A one-block pool forces handoff before waiting for return; cover single-Writer, list and multi-copy-thread paths.
template<std::uint32_t Count> static void queue_forwarding(unsigned layout) {
    WriterSourceConfig source;
    source.block_size = 60;
    source.pool_min = source.pool_max = 1;
    source.backpressure.mode = BackpressureMode::Block;
    source.backpressure.wait_get_timeout = 2s;
    auto w0 = make_writer<Writer<Count, true, std::uint16_t, void>>(source);
    source.thread_id = 1;
    std::unique_ptr<decltype(w0)> w1;
    if (layout >= 2) w1 = std::make_unique<decltype(w0)>(source);
    TwoBufQueueOptions options;
    options.read_config = config();
    options.read_config.is_scan_current = false;
    options.read_layer1_thread_count = layout == 3 ? 2 : 1;
    options.output_block_size = 120;
    options.output_frame_capacity = 4;
    options.output_pool_min = options.output_pool_max = 4;
    std::array<unsigned, 2> received{};
    bool valid = true;
    auto callback = [&](std::int32_t id, const std::byte* data, std::size_t size) noexcept {
        unsigned value = 0;
        if (size == sizeof(value)) std::memcpy(&value, data, sizeof(value));
        if (id < 0 || id > 1) { valid = false; return; }
        valid &= size == sizeof(value) && value == received[id];
        ++received[id];
    };
    using Queue = ::TwoBufQueue::TwoBufQueue<decltype(callback), decltype(w0)>;
    std::unique_ptr<Queue> queue;
    if (layout == 0) queue = std::make_unique<Queue>(callback, w0, options);
    else if (layout == 1)
        queue = std::make_unique<Queue>(callback, std::initializer_list<decltype(w0)*>{&w0}, options);
    else queue = std::make_unique<Queue>(callback, std::initializer_list<decltype(w0)*>{&w0, w1.get()}, options);
    CHECK(queue->started());
    constexpr unsigned total = 103; // The final block contains a partial tail.
    for (unsigned i = 0; i < total; ++i) {
        auto send = [&](auto& writer) {
            CHECK(i % 2 ? writer.write(i) : writer.write_mem(&i, sizeof(i)));
            if (i % 7 == 0) writer.flush(); // Even flush must not invoke current callbacks.
        };
        send(w0);
        if (w1) send(*w1);
    }
    w0.finish(); if (w1) w1->finish(); queue->stop();
    CHECK(valid && received[0] == total && received[1] == (layout >= 2 ? total : 0));
    CHECK(queue->error_code() == ReadError::none && queue->stats().saved_partial_frames == 0);
    CHECK(queue->stats().saved_data_bytes == (layout >= 2 ? 2 : 1) * total * 6);
}

static void unframed_batch() {
    WriterSourceConfig source;
    source.block_size = 32; source.pool_min = source.pool_max = 1;
    source.backpressure.mode = BackpressureMode::Block;
    auto writer = make_writer<Writer<block_only, false, std::uint16_t, void>>(source);
    TwoBufQueueOptions options;
    options.read_config.is_scan_current = false;
    options.output_block_size = 64;
    unsigned received = 0;
    bool valid = true;
    auto callback = [&](const DataBatch& batch) noexcept {
        for (const auto& frame : batch.frames) {
            valid &= frame.size % sizeof(unsigned) == 0 && frame.thread_id == 0;
            for (std::size_t offset = 0; offset + sizeof(unsigned) <= frame.size; offset += sizeof(unsigned)) {
                unsigned value = 0;
                std::memcpy(&value, frame.data + offset, sizeof(value));
                valid &= value == received++;
            }
        }
        return true;
    };
    TwoBufQueueBatch queue(callback, writer, options);
    CHECK(queue.started());
    for (unsigned i = 0; i < 103; ++i)
        CHECK(i % 2 ? writer.write(i) : writer.write_mem(&i, sizeof(i)));
    writer.finish(); queue.stop();
    CHECK(valid && received == 103 && queue.error_code() == ReadError::none);
    CHECK(queue.stats().saved_partial_frames == 0);
}

static void sequenced_records() {
    WriterSourceConfig source;
    source.block_size = 30; source.pool_min = source.pool_max = 1;
    source.backpressure.mode = BackpressureMode::Block;
    auto writer = make_writer<SeqWriter<std::uint32_t, block_only, true, std::uint16_t, void>>(source);
    TwoBufQueueOptions options;
    options.read_config.is_scan_current = false;
    unsigned received = 0;
    bool valid = true;
    auto callback = [&](std::int32_t id, std::uint32_t seq, const std::byte* data, std::size_t size) noexcept {
        unsigned value = 0;
        if (size == sizeof(value)) std::memcpy(&value, data, sizeof(value));
        valid &= id == 0 && seq == received && size == sizeof(value) && value == received++;
    };
    ::TwoBufQueue::TwoBufQueue queue(callback, writer, options);
    CHECK(queue.started());
    for (unsigned i = 0; i < 103; ++i)
        CHECK(i % 2 ? writer.write(i, i) : writer.write_mem(i, &i, sizeof(i)));
    writer.finish(); queue.stop();
    CHECK(valid && received == 103 && queue.error_code() == ReadError::none);
    CHECK(queue.stats().saved_partial_frames == 0);
}

// Capacity counting must support variable record lengths; cover empty payloads, oversized rejection, pressure recovery and finish.
static void capacity_edges() {
    WriterFixture source(34, 2, 2);
    Writer<block_only, true, std::uint16_t, void> writer(&source.queue, 0);
    const auto* first = writer.source_block()->data();
    CHECK(writer.write(std::uint64_t{7})); // 10 bytes.
    CHECK(writer.write(std::uint32_t{9})); // 6 bytes.
    const std::array<std::byte, 2> data{std::byte{1}, std::byte{2}};
    CHECK(writer.write_mem(data.data(), data.size())); // 4 bytes.
    CHECK(writer.published_write_ptr() == first);
    CHECK(!writer.write_mem(data.data(), 33)); // Cannot fit even an entire block; leave capacity unchanged.
    CHECK(writer.write_mem(nullptr, 0)); // An empty payload still has a length prefix.
    writer.flush();
    CHECK(writer.published_write_ptr() == first + 22);
    CHECK(writer.write(std::array<std::byte, 12>{})); // Switch blocks; 14 bytes.
    const auto* second = writer.source_block()->data();
    CHECK(second != first && writer.published_write_ptr() == second);
    writer.finish_block();
    CHECK(!writer.write(3u)); // Two handed-off blocks have not returned; safely reject the write.
    while (auto claim = source.queue.try_pop()) {
        if (claim.get()) CHECK(claim->consume(claim->readable().size()));
        source.queue.finish(claim);
    }
    CHECK(source.all_released());
    CHECK(writer.write(4u)); // Reacquire after backpressure and reset remaining capacity.
    writer.flush();
    CHECK(writer.source_block()->readable().size() == 6);
    writer.finish();
    CHECK(!writer.write(5u) && !writer.write_mem(nullptr, 0));
    writer.finish(); writer.finish_block(); writer.flush();
}

// This policy also permits scanning current: invisible before flush, immediately readable afterward.
static void current_flush() {
    WriterFixture source(64, 2, 2);
    Writer<block_only, true, std::uint16_t, void> writer(&source.queue, 0);
    Probe probe; AutoResetEvent event; std::atomic<bool> cancel{false};
    SingleReadService service(config(), probe, event, writer);
    service.prepare_start();
    CHECK(!service.read_once(true, cancel).pending);
    CHECK(writer.write(0u));
    CHECK(!service.read_once(true, cancel).pending && probe.accepted == 0);
    writer.flush();
    CHECK(!service.read_once(true, cancel).pending && probe.accepted == 1);
    CHECK(writer.write(1u)); writer.flush();
    CHECK(!service.read_once(true, cancel).pending && probe.accepted == 2);
    writer.finish();
    CHECK(!service.read_once(true, cancel).pending && source.all_released());
    probe.verify(0, 2);
    CHECK(probe.current_calls == 2 && service.error_code() == ReadError::none);
    service.detach_completed_callbacks();
}

int main() {
    CHECK(ReadConfig{}.is_scan_current && TwoBufQueueOptions{}.read_config.is_scan_current);
    completed_only_service<true, 1>(); completed_only_service<false, 1>();
    completed_only_service<true, block_only>(); completed_only_service<false, block_only>();
    for (unsigned layout = 0; layout != 4; ++layout) {
        queue_forwarding<1>(layout); queue_forwarding<block_only>(layout);
    }
    unframed_batch(); sequenced_records();
    capacity_edges(); current_flush();
    std::puts("16 writer capacity and reader scenarios passed");
}
