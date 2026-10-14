#include "../support/read_test_support.hpp"
#include "TwoBufQueue/src/read/read_layer1/single_writer/read_worker.hpp"
using namespace read_test;

static ReadConfig slow_config() {
    auto cfg = config(); cfg.idle_rescan_interval = 1h; cfg.idle_rescan_max_interval = 1h; return cfg;
}
struct SingleRig {
    WriterFixture source;
    Writer<> writer;
    Probe probe;
    AutoResetEvent event;
    SingleReadService<Probe> service;
    std::atomic<bool> cancel{false};
    explicit SingleRig(std::size_t bytes = 24, std::size_t blocks = 4)
        : source(bytes, blocks, blocks), writer(&source.queue, 7, source.backpressure),
          service(slow_config(), probe, event, writer) {}
    ReadResult tick(bool force = true) { return service.read_once(force, cancel); }
    void drain() {
        probe.accept = true; probe.decide = {}; probe.begin_complete_hook = {};
        probe.begin_current_hook = {}; probe.end_hook = {}; cancel = false; writer.finish();
        CHECK(!tick().pending && source.all_released());
    }
};
static void complete_then_current() {
    SingleRig r(6); CHECK(r.writer.write(0u) && r.writer.write(1u));
    CHECK(!r.tick().pending);
    r.probe.expect({{'B'}, {'F',7,6}, {'E'}, {'b'}, {'C',7,6}, {'e'}});
    r.drain(); r.probe.verify(7,2);
}
static void rejection_and_transfer() {
    SingleRig r(6); CHECK(r.writer.write(0u) && r.writer.write(1u));
    r.probe.accept = false; CHECK(r.tick().pending); CHECK(r.tick().pending);
    CHECK(r.service.stats().memory_full_events == 2 && r.service.stats().memory_backpressured_frames == 1);
    CHECK(r.probe.current_calls == 0 && r.service.pending_finished_count() == 1);
    auto pending = r.service.take_pending(); CHECK(pending);
    AutoResetEvent event; SingleReadService next(slow_config(),r.probe,event,r.writer);
    CHECK(next.adopt_pending(pending) && !pending);
    r.probe.accept = true; r.writer.finish();
    CHECK(!next.read_once(true,r.cancel).pending && r.source.all_released());
    r.probe.verify(7,2);
}
static void current_snapshot_and_notification() {
    SingleRig r; r.service.prepare_start(); CHECK(!r.tick().pending);
    CHECK(r.writer.write(0u)); r.writer.flush(); CHECK(r.event.try_wait());
    r.probe.decide = [&](char phase, SpscWriteBlock*, const std::byte*, std::size_t size) {
        CHECK(phase == 'C' && size == 6);
        if (r.probe.accepted == 0) { CHECK(r.writer.write(1u)); r.writer.flush(); }
        return true;
    };
    CHECK(!r.tick().pending && r.probe.accepted == 1 && r.writer.source_block()->read_pos() == 6);
    CHECK(r.event.try_wait()); // A notification raised inside the callback must survive this read round.
    CHECK(!r.tick().pending && r.probe.accepted == 2 && !r.event.try_wait());
    r.drain(); r.probe.verify(7,2);
}
static void cancelled_and_current_rejection() {
    SingleRig r; CHECK(!r.tick().pending); CHECK(r.writer.write(0u));
    CHECK(r.tick(false).pending && r.probe.accepted == 0); // The scan deadline is not due yet.
    r.cancel = true; CHECK(r.tick().pending && r.probe.accepted == 0);
    r.cancel = false; r.probe.accept = false; CHECK(r.tick().pending);
    CHECK(r.writer.source_block()->read_pos() == 0);
    r.probe.accept = true; CHECK(!r.tick().pending); r.drain(); r.probe.verify(7,1);
}
template<bool Accept> static void completion_during_current() {
    for (unsigned round = 0; round != 30; ++round) {
        SingleRig r(12); CHECK(r.writer.write(0u)); auto* original = r.writer.source_block();
        Gate gate;
        r.probe.decide = [&](char phase, SpscWriteBlock* block, const std::byte*, std::size_t size) {
            CHECK(phase == 'C' && block == original && size == 6); gate.pause(); return Accept;
        };
        ReadResult result; std::thread reader([&] { result = r.tick(); }); gate.wait();
        CHECK(r.writer.write(1u) && r.writer.write(2u) && r.writer.write(3u)); r.writer.finish();
        gate.release(); reader.join();
        CHECK(result.pending && original->read_pos() == (Accept ? 6u : 0u) && !original->is_free());
        r.drain(); r.probe.verify(7,4); CHECK(r.service.stats().saved_data_bytes == 24);
    }
}
static void accepted_current() { completion_during_current<true>(); }
static void rejected_current() { completion_during_current<false>(); }
static void prefix_append() {
    SingleRig r(6); CHECK(r.writer.write(0u) && r.writer.write(1u));
    Gate gate; r.probe.begin_complete_hook = [&] { gate.pause(); };
    ReadResult result; std::thread reader([&] { result = r.tick(); }); gate.wait();
    CHECK(r.writer.write(2u)); gate.release(); reader.join();
    CHECK(result.pending && r.probe.accepted == 2 && r.source.queue.pending_count() == 1);
    r.drain(); r.probe.verify(7,3);
}
template<unsigned Publish> static void concurrent(std::size_t blocks) {
    WriterFixture source(30,blocks,blocks,WriterBackpressureConfig{},0,2,4);
    Writer<Publish> writer(&source.queue,0); Probe probe; unsigned attempts = 0;
    probe.decide = [&](char, SpscWriteBlock*, const std::byte*, std::size_t) { return ++attempts % 3 != 0; };
    SingleReadWorker worker(config(),probe,writer); CHECK(worker.start());
    std::thread producer([&] {
        for (unsigned i = 0; i != 5000; ++i) CHECK(wait_until([&] { return writer.write(i); }));
        writer.finish();
    });
    producer.join(); worker.stop(); probe.verify(0,5000);
    CHECK(source.all_released() && worker.pending_finished_count() == 0);
    CHECK(worker.error_code() == ReadError::none && worker.stats().saved_data_bytes == 30000);
    CHECK(worker.stats().memory_full_events != 0 && !source.queue.has_read_available_callback());
}
static void immediate_concurrent() { concurrent<1>(4); }
static void single_pool_batched() { concurrent<4>(1); }
static void worker_flush_finish() {
    WriterFixture source(64,2,2); Writer<4> writer(&source.queue,0); Probe probe; Gate initial;
    probe.end_hook = [&] { if (!initial.entered.load()) initial.pause(); };
    SingleReadWorker worker(slow_config(),probe,writer); CHECK(worker.start()); initial.wait();
    CHECK(writer.write(0u)); writer.flush(); initial.release();
    CHECK(wait_until([&] { return probe.accepted == 1; }));
    probe.accept = false; CHECK(writer.write(1u)); writer.flush();
    CHECK(wait_until([&] { return worker.stats().memory_full_events != 0; }));
    probe.accept = true; worker.notify_memory_available();
    CHECK(wait_until([&] { return probe.accepted == 2; }));
    CHECK(writer.write(2u)); writer.finish();
    CHECK(wait_until([&] { return probe.accepted == 3; }));
    worker.stop(); probe.verify(0,3); CHECK(source.all_released());
}
static void current_without_send() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe; Gate initial;
    probe.end_hook = [&] { if (!initial.entered.load()) initial.pause(); };
    SingleReadWorker worker(config(),probe,writer); CHECK(worker.start()); initial.wait();
    CHECK(writer.write(0u)); initial.release(); CHECK(wait_until([&] { return probe.accepted == 1; }));
    CHECK(source.in_use_count() == 1); // consumer has not recycled the still-current block
    writer.finish(); worker.stop(); probe.verify(0,1);
    CHECK(source.all_released() && worker.stats().saved_partial_frames == 1);
}
static void notify_retry() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe; probe.accept = false;
    CHECK(writer.write(0u)); writer.finish();
    auto cfg = config(); cfg.work_wait_min_interval = 30s;
    SingleReadWorker worker(cfg,probe,writer); CHECK(worker.start());
    CHECK(wait_until([&] { return worker.stats().memory_full_events != 0; }));
    probe.accept = true; worker.notify_memory_available();
    CHECK(wait_until([&] { return probe.accepted == 1; })); // must not wait the configured 30 seconds
    worker.stop(); probe.verify(0,1); CHECK(source.all_released());
}
static void fast_stop_restart() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe; probe.accept = false;
    CHECK(writer.write(0u)); auto* block = writer.source_block(); writer.finish();
    SingleReadWorker worker(config(),probe,writer); CHECK(worker.start());
    CHECK(worker.start());
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
    SingleReadWorker worker(cfg,probe,writer); CHECK(worker.start());
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
    auto pending = worker.take_pending();
    CHECK(pending && worker.pending_finished_count() == 0);
    probe.accept = true; AutoResetEvent event; SingleReadService service(config(),probe,event,source.queue,0);
     const std::atomic<bool> cancel{false};
    CHECK(service.adopt_pending(pending) && !pending);
    CHECK(!service.read_once(true,cancel).pending && source.all_released()); probe.verify(0,1);
}
static void interrupt_slow_stop() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe; probe.accept = false;
    CHECK(writer.write(0u)); writer.finish();
    auto cfg = config(); cfg.finish_timeout = 30s; cfg.work_wait_min_interval = 30s;
    SingleReadWorker worker(cfg,probe,writer); CHECK(worker.start());
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

static void restart_current() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe; Gate initial;
    probe.end_hook = [&] { if (!initial.entered.load()) initial.pause(); };
    SingleReadWorker worker(slow_config(),probe,writer); CHECK(worker.start()); initial.wait();
    worker.request_stop(ReadStopMode::fast); initial.release(); worker.stop(ReadStopMode::fast);
    CHECK(writer.write(0u)); // No event and a one-hour scan deadline; restart must still read existing data.
    CHECK(worker.start()); CHECK(wait_until([&] { return probe.accepted == 1; }));
    writer.finish(); worker.stop(); probe.verify(0,1); CHECK(source.all_released());
}
static void begin_stop_and_deadline() {
    SingleRig r(6); CHECK(r.writer.write(0u) && r.writer.write(1u));
    r.probe.begin_complete_hook = [&] { r.cancel = true; };
    CHECK(r.tick().pending && r.probe.completed_calls == 0 && r.probe.current_calls == 0);
    r.probe.expect({{'B'}, {'E'}, {'e'}});
    r.cancel = false; r.probe.begin_complete_hook = {}; r.probe.events.clear();
    CHECK(r.service.read_once(true,r.cancel,AutoResetEvent::Clock::now()).pending);
    r.probe.expect({{'e'}});
    r.probe.begin_current_hook = [&] { r.cancel = true; };
    CHECK(r.tick().pending && r.probe.completed_calls == 1 && r.probe.current_calls == 0);
    r.drain(); r.probe.verify(7,2);
}
static void validation() {
    WriterFixture source(64,1,1); Writer<> writer(&source.queue,0); Probe probe;
    { SingleReadWorker invalid(config(),probe,source.queue,-1);
      CHECK(!invalid.start() && !source.queue.has_read_available_callback()); }
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
        SingleReadWorker worker(cfg,probe,writer);
        CHECK(!worker.start() && worker.error_code() == ReadError::invalid_start_state);
        CHECK(!worker.running() && !source.queue.has_read_available_callback());
    }
    writer.finish();
}
int main(int argc, char** argv) {
    return run_cases(argc, argv, {
        {"complete_then_current",complete_then_current}, {"rejection_and_transfer",rejection_and_transfer},
        {"current_snapshot_and_notification",current_snapshot_and_notification},
        {"cancelled_and_current_rejection",cancelled_and_current_rejection},
        {"accepted_current",accepted_current}, {"rejected_current",rejected_current}, {"prefix_append",prefix_append},
        {"immediate_concurrent",immediate_concurrent}, {"single_pool_batched",single_pool_batched},
        {"worker_flush_finish",worker_flush_finish}, {"current_without_send",current_without_send},
        {"notify_retry",notify_retry}, {"fast_stop_restart",fast_stop_restart}, {"slow_stop_timeout",slow_stop_timeout},
        {"interrupt_slow_stop",interrupt_slow_stop}, {"restart_current",restart_current},
        {"begin_stop_and_deadline",begin_stop_and_deadline}, {"validation",validation}});
}
