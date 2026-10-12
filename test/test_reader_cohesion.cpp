#include "read_test_support.hpp"
using namespace read_test;

template<unsigned Publish> static void concurrent(unsigned writer_count, unsigned per_writer, std::size_t blocks) {
    Probe probe;
    unsigned attempts = 0;
    probe.decide = [&](char, SpscWriteBlock*, const std::byte*, std::size_t) { return ++attempts % 3 != 0; };
    std::vector<std::unique_ptr<WriterFixture>> sources;
    std::vector<std::unique_ptr<Writer<Publish>>> writers;
    for (unsigned i = 0; i != writer_count; ++i) {
        sources.push_back(std::make_unique<WriterFixture>(30, blocks, blocks, WriterBackpressureConfig{}, 0, 2, 4));
        writers.push_back(std::make_unique<Writer<Publish>>(&sources.back()->queue, static_cast<int>(i)));
    }
    ReadWorker worker(config(), probe);
    for (const auto& writer : writers) CHECK(worker.add_writer(writer.get()));
    CHECK(worker.start());
    std::vector<std::thread> threads;
    for (unsigned i = 0; i != writer_count; ++i) threads.emplace_back([&, i] {
        for (unsigned value = 0; value != per_writer; ++value)
            CHECK(wait_until([&] { return writers[i]->write(value); }));
        writers[i]->finish();
    });
    for (auto& thread : threads) thread.join();
    worker.stop();
    CHECK(worker.error_code() == ReadError::none && worker.pending_finished_count() == 0);
    CHECK(worker.stats().saved_data_bytes == static_cast<std::uint64_t>(writer_count) * per_writer * 6);
    CHECK(worker.stats().memory_full_events != 0);
    for (unsigned i = 0; i != writer_count; ++i) {
        probe.verify(static_cast<int>(i), per_writer); CHECK(sources[i]->all_released());
        CHECK(!sources[i]->queue.has_read_available_callback());
    }
}
static void immediate_concurrent() { concurrent<1>(3,5000,4); }
static void batched_concurrent() { concurrent<4>(3,5000,4); }
static void single_block_concurrent() { concurrent<4>(2,2000,1); }

static void current_without_send() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe;
    ReadWorker worker(config(),probe); CHECK(worker.add_writer(&writer) && worker.start());
    CHECK(writer.write(0u)); CHECK(wait_until([&] { return probe.accepted == 1; }));
    CHECK(source.in_use_count() == 1); // consumer has not recycled the still-current block
    writer.finish(); worker.stop(); probe.verify(0,1);
    CHECK(source.all_released() && worker.stats().saved_partial_frames == 1);
}
static void notify_retry() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe; probe.accept = false;
    CHECK(writer.write(0u)); writer.finish();
    auto cfg = config(); cfg.work_wait_min_interval = 30s;
    ReadWorker worker(cfg,probe); CHECK(worker.add_writer(&writer) && worker.start());
    CHECK(wait_until([&] { return worker.stats().memory_full_events != 0; }));
    probe.accept = true; worker.notify_memory_available();
    CHECK(wait_until([&] { return probe.accepted == 1; })); // must not wait the configured 30 seconds
    worker.stop(); probe.verify(0,1); CHECK(source.all_released());
}
static void fast_stop_restart() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe; probe.accept = false;
    CHECK(writer.write(0u)); auto* block = writer.source_block(); writer.finish();
    ReadWorker worker(config(),probe); CHECK(worker.add_writer(&writer) && worker.start());
    CHECK(worker.start()); CHECK(!worker.add_writer(&writer));
    CHECK(wait_until([&] { return worker.stats().memory_full_events != 0; }));
    worker.stop(ReadStopMode::fast);
    CHECK(!worker.running() && worker.pending_finished_count() == 1 && block->read_pos() == 0);
    CHECK(!source.queue.has_read_available_callback() && source.queue.empty());
    probe.accept = true; CHECK(worker.start()); worker.stop(); worker.stop();
    CHECK(!worker.running() && source.all_released()); probe.verify(0,1);
}
static void slow_stop_timeout() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe; probe.accept = false;
    CHECK(writer.write(0u)); writer.finish();
    auto cfg = config(); cfg.finish_timeout = 30ms;
    ReadWorker worker(cfg,probe); CHECK(worker.add_writer(&writer) && worker.start());
    CHECK(wait_until([&] { return worker.stats().memory_full_events != 0; }));
    std::atomic<bool> done{false};
    std::thread notifications([&] { while (!done.load()) { worker.notify_memory_available(); std::this_thread::yield(); } });
    const auto begin = std::chrono::steady_clock::now();
    worker.stop(); done = true; notifications.join();
    CHECK(std::chrono::steady_clock::now() - begin < 3s);
    CHECK(worker.error_code() == ReadError::finish_timeout && !worker.running());
    CHECK(source.queue.empty() && worker.pending_finished_count() == 1 && source.in_use_count() == 1);
    CHECK(!source.queue.has_read_available_callback());
    // Timeout does not discard data; explicitly transfer the popped pending to a new Service.
    auto pending = worker.take_pending(&source.queue);
    CHECK(pending && worker.pending_finished_count() == 0);
    probe.accept = true; AutoResetEvent event; ReadService service(config(),probe,event);
    CHECK(service.add_queue(&source.queue,0)); const std::atomic<bool> cancel{false};
    CHECK(service.adopt_pending(&source.queue,pending) && !pending);
    CHECK(!service.read_once(true,cancel).pending && source.all_released()); probe.verify(0,1);
}
static void interrupt_slow_stop() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe; probe.accept = false;
    CHECK(writer.write(0u)); writer.finish();
    auto cfg = config(); cfg.finish_timeout = 30s; cfg.work_wait_min_interval = 30s;
    ReadWorker worker(cfg,probe); CHECK(worker.add_writer(&writer) && worker.start());
    CHECK(wait_until([&] { return worker.stats().memory_full_events != 0; }));
    const auto calls = probe.completed_calls.load();
    std::atomic<bool> stopped{false};
    std::thread stopper([&] { worker.stop(); stopped = true; });
    CHECK(wait_until([&] { return probe.completed_calls.load() > calls; }));
    worker.request_stop(ReadStopMode::fast);
    CHECK(wait_until([&] { return stopped.load(); })); stopper.join();
    CHECK(worker.error_code() != ReadError::finish_timeout && !source.all_released());
    probe.accept = true; CHECK(worker.start()); worker.stop();
    CHECK(source.all_released()); probe.verify(0,1);
}
static void validation() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe;
    { ReadWorker empty(config(),probe); CHECK(!empty.start() && empty.error_code() == ReadError::invalid_start_state); }
    for (unsigned i = 0; i != 7; ++i) {
        auto cfg = config();
        switch (i) {
        case 0: cfg.idle_rescan_interval = 0ns; break;
        case 1: cfg.idle_rescan_interval = -1ns; break;
        case 2: cfg.idle_rescan_max_interval = 1ns; break;
        case 3: cfg.finish_timeout = 0ms; break;
        case 4: cfg.work_wait_min_interval = 0ms; break;
        case 5: cfg.finish_retry_interval = 0ms; break;
        default: cfg.finish_timeout = std::chrono::milliseconds::max(); break;
        }
        ReadWorker worker(cfg,probe); CHECK(worker.add_writer(&writer));
        CHECK(!worker.start() && worker.error_code() == ReadError::invalid_start_state);
        CHECK(!worker.running() && !source.queue.has_read_available_callback());
    }
    writer.finish();
}
int main(int argc, char** argv) {
    return run_cases(argc, argv, {{"immediate_concurrent",immediate_concurrent}, {"batched_concurrent",batched_concurrent},
        {"single_block_concurrent",single_block_concurrent}, {"current_without_send",current_without_send},
        {"notify_retry",notify_retry}, {"fast_stop_restart",fast_stop_restart},
        {"slow_stop_timeout",slow_stop_timeout}, {"interrupt_slow_stop",interrupt_slow_stop}, {"validation",validation}});
}
