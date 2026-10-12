#include "read_test_support.hpp"
using namespace read_test;

static ReadConfig slow_scan_config() {
    auto cfg = config();
    cfg.idle_rescan_interval = 1h;
    cfg.idle_rescan_max_interval = 1h;
    return cfg;
}

struct Sources {
    std::vector<std::unique_ptr<WriterFixture>> sources;
    std::vector<std::unique_ptr<Writer<>>> writers;
    Probe probe;
    AutoResetEvent event;
    ReadService<Probe> service{slow_scan_config(), probe, event};
    std::atomic<bool> cancel{false};
    explicit Sources(unsigned count, std::size_t bytes = 24) {
        for (unsigned i = 0; i != count; ++i) {
            sources.push_back(std::make_unique<WriterFixture>(bytes, 4, 4));
            writers.push_back(std::make_unique<Writer<>>(&sources.back()->queue, static_cast<int>(i)));
            CHECK(service.add_writer(writers.back().get()));
        }
        service.prepare_start();
        CHECK(!service.read_once(false, cancel).pending);
    }
    // Establish current blocks, consume their start notifications and arm distant scan deadlines.
    void arm() {
        for (auto& writer : writers) CHECK(writer->write(0u));
        CHECK(!service.read_once(true, cancel).pending);
        probe.events.clear(); probe.records.clear(); probe.accepted = 0;
        event.reset();
    }
    void drain() {
        probe.accept = true; probe.decide = {}; probe.begin_complete_hook = {};
        probe.begin_current_hook = {}; probe.end_hook = {}; cancel = false;
        for (auto& writer : writers) writer->finish();
        CHECK(!service.read_once(true, cancel).pending);
        for (const auto& source : sources) CHECK(source->all_released());
    }
};

static void selective_and_coalesced() {
    Sources rig(3); rig.arm();
    for (auto& writer : rig.writers) CHECK(writer->write(1u));
    rig.writers[1]->flush(); rig.writers[1]->flush();
    CHECK(rig.event.try_wait() && !rig.event.try_wait());
    CHECK(!rig.service.read_notified(rig.cancel).pending);
    rig.probe.expect({{'b'}, {'C', 1, 6}, {'e'}});
    CHECK(rig.probe.values(0).empty() && rig.probe.values(2).empty());
    CHECK(rig.service.next_scan_deadline() > ReadItem::SteadyClock::duration::zero());
    // A regular scan before the deadlines must still leave the other current blocks alone.
    CHECK(rig.service.read_once(false, rig.cancel).pending);
    CHECK(rig.probe.accepted == 1);
    rig.writers[0]->flush(); rig.writers[2]->flush();
    CHECK(rig.event.try_wait());
    CHECK(!rig.service.read_notified(rig.cancel).pending);
    CHECK(rig.probe.values(0) == std::vector<unsigned>{1});
    CHECK(rig.probe.values(2) == std::vector<unsigned>{1});
    rig.drain();
}

static void completed_before_current() {
    Sources rig(1, 6);
    CHECK(rig.writers[0]->write(0u) && rig.writers[0]->write(1u));
    rig.probe.events.clear();
    CHECK(!rig.service.read_notified(rig.cancel).pending);
    rig.probe.expect({{'B'}, {'F', 0, 6}, {'E'}, {'b'}, {'C', 0, 6}, {'e'}});
    rig.probe.verify(0, 2);
    // finish also reaches the same source callback and recycles the consumed current block.
    rig.event.reset(); rig.writers[0]->finish();
    CHECK(rig.event.try_wait());
    CHECK(!rig.service.read_notified(rig.cancel).pending && rig.sources[0]->all_released());
    rig.drain();
}

static void full_scan_with_notification() {
    Sources rig(2); rig.arm();
    CHECK(rig.writers[0]->write(1u) && rig.writers[1]->write(1u));
    rig.writers[0]->flush();
    // Startup, timeout or a coincident deadline may choose the full path after a flush.
    CHECK(rig.service.read_once(false, rig.cancel).pending);
    CHECK(rig.probe.values(0) == std::vector<unsigned>{1});
    CHECK(rig.probe.values(1).empty());
    rig.drain();
}

static void full_scan_rejection_retains_notifications() {
    Sources rig(2, 6);
    CHECK(rig.writers[0]->write(0u) && rig.writers[0]->write(1u));
    CHECK(rig.writers[1]->write(0u)); rig.writers[1]->flush();
    rig.probe.accept = false;
    CHECK(rig.service.read_once(false, rig.cancel).pending);
    rig.probe.accept = true;
    CHECK(!rig.service.read_notified(rig.cancel).pending && rig.probe.accepted == 3);
    rig.probe.verify(0, 2); rig.probe.verify(1, 1);
    rig.drain();
}

static void batched_tail_flush() {
    WriterFixture source(64, 2, 2); Writer<4> writer(&source.queue, 0);
    Probe probe; AutoResetEvent event; std::atomic<bool> cancel{false};
    ReadService service(slow_scan_config(), probe, event);
    CHECK(service.add_writer(&writer)); service.prepare_start();
    CHECK(!service.read_once(false, cancel).pending);
    CHECK(writer.write(0u));
    CHECK(!service.read_notified(cancel).pending && probe.accepted == 0);
    writer.flush(); CHECK(event.try_wait());
    CHECK(!service.read_notified(cancel).pending && probe.accepted == 1);
    writer.finish(); CHECK(!service.read_once(true, cancel).pending);
    probe.verify(0, 1); CHECK(source.all_released());
}

static void notify_during_read() {
    Sources rig(1); rig.arm();
    CHECK(rig.writers[0]->write(1u)); rig.writers[0]->flush();
    bool appended = false;
    rig.probe.decide = [&](char phase, SpscWriteBlock*, const std::byte*, std::size_t size) {
        CHECK(phase == 'C' && size == 6);
        if (!appended) {
            appended = true;
            CHECK(rig.writers[0]->write(2u)); rig.writers[0]->flush();
        }
        return true;
    };
    CHECK(rig.event.try_wait());
    CHECK(!rig.service.read_notified(rig.cancel).pending && rig.probe.accepted == 1);
    CHECK(rig.event.try_wait());
    CHECK(!rig.service.read_notified(rig.cancel).pending && rig.probe.accepted == 2);
    CHECK(rig.probe.values(0) == (std::vector<unsigned>{1, 2}));
    rig.drain();
}

static void multiple_words() {
    Sources rig(70); rig.arm();
    CHECK(rig.writers[0]->write(1u) && rig.writers[64]->write(1u) && rig.writers[69]->write(1u));
    rig.writers[0]->flush(); rig.writers[64]->flush(); rig.writers[69]->flush();
    CHECK(!rig.service.read_notified(rig.cancel).pending && rig.probe.accepted == 3);
    CHECK(rig.probe.values(64) == std::vector<unsigned>{1});
    CHECK(rig.probe.values(69) == std::vector<unsigned>{1});
    rig.drain();
}

static void rejected_and_cancelled() {
    Sources rig(2); rig.arm();
    for (auto& writer : rig.writers) { CHECK(writer->write(1u)); writer->flush(); }
    rig.probe.accept = false;
    CHECK(rig.event.try_wait());
    CHECK(rig.service.read_notified(rig.cancel).pending && rig.probe.accepted == 0);
    CHECK(!rig.event.try_wait()); // Rejection retains sources without a self-waking busy loop.
    rig.writers[0]->flush(); CHECK(rig.event.try_wait()); // A retained bit must not suppress new signals.
    rig.cancel = true;
    CHECK(rig.service.read_notified(rig.cancel).pending);
    rig.cancel = false; rig.probe.accept = true;
    CHECK(!rig.service.read_notified(rig.cancel).pending && rig.probe.accepted == 2);
    rig.drain();
}

static void worker_flush_and_memory_retry() {
    WriterFixture source0(64, 2, 2), source1(64, 2, 2);
    Writer<> writer0(&source0.queue, 0), writer1(&source1.queue, 1);
    Probe probe; Gate initial;
    probe.end_hook = [&] { if (!initial.entered.load()) initial.pause(); };
    ReadWorker worker(slow_scan_config(), probe);
    CHECK(worker.add_writer(&writer0) && worker.add_writer(&writer1) && worker.start());
    initial.wait();
    CHECK(writer0.write(0u) && writer1.write(0u));
    writer0.flush(); writer1.flush();
    initial.release();
    CHECK(wait_until([&] { return probe.accepted == 2; }));
    // Both current blocks exist; an ordinary write now emits no source notification.
    CHECK(writer0.write(1u) && writer1.write(1u));
    probe.accept = false; writer0.flush();
    CHECK(wait_until([&] { return worker.stats().memory_full_events != 0; }));
    probe.accept = true; worker.notify_memory_available();
    CHECK(wait_until([&] { return probe.accepted == 4; })); // Space notification forces a retry despite 1h scan interval.
    writer0.finish(); writer1.finish(); worker.stop();
    probe.verify(0, 2); probe.verify(1, 2);
    CHECK(source0.all_released() && source1.all_released() && worker.error_code() == ReadError::none);
}

static void worker_selective_and_restart() {
    WriterFixture source0(64, 2, 2), source1(64, 2, 2);
    Writer<> writer0(&source0.queue, 0), writer1(&source1.queue, 1);
    Probe probe; Gate initial, selected;
    std::atomic<unsigned> ends{0};
    probe.end_hook = [&] {
        if (ends.fetch_add(1) == 0) initial.pause();
        else if (probe.accepted == 3 && !selected.entered.load()) selected.pause();
    };
    ReadWorker worker(slow_scan_config(), probe);
    CHECK(worker.add_writer(&writer0) && worker.add_writer(&writer1) && worker.start());
    initial.wait(); CHECK(writer0.write(0u) && writer1.write(0u));
    writer0.flush(); writer1.flush(); initial.release();
    CHECK(wait_until([&] { return probe.accepted == 2; }));
    CHECK(writer0.write(1u) && writer1.write(1u)); writer0.flush();
    selected.wait();
    CHECK(probe.values(0) == (std::vector<unsigned>{0, 1}));
    CHECK(probe.values(1) == std::vector<unsigned>{0});
    selected.release(); worker.stop(ReadStopMode::fast);
    // Registering another source may relocate QueueEntry storage; restart must refresh callback contexts.
    WriterFixture source2(64, 2, 2); Writer<> writer2(&source2.queue, 2);
    CHECK(worker.add_writer(&writer2) && worker.start());
    writer1.flush(); CHECK(writer2.write(0u)); writer2.flush();
    CHECK(wait_until([&] { return probe.accepted == 5; }));
    writer0.finish(); writer1.finish(); writer2.finish(); worker.stop();
    probe.verify(0, 2); probe.verify(1, 2); probe.verify(2, 1);
    CHECK(source0.all_released() && source1.all_released() && source2.all_released());
}

int main(int argc, char** argv) {
    return run_cases(argc, argv, {{"selective_and_coalesced", selective_and_coalesced},
        {"completed_before_current", completed_before_current}, {"notify_during_read", notify_during_read},
        {"full_scan_with_notification", full_scan_with_notification},
        {"full_scan_rejection_retains_notifications", full_scan_rejection_retains_notifications},
        {"batched_tail_flush", batched_tail_flush},
        {"multiple_words", multiple_words}, {"rejected_and_cancelled", rejected_and_cancelled},
        {"worker_flush_and_memory_retry", worker_flush_and_memory_retry},
        {"worker_selective_and_restart", worker_selective_and_restart}});
}
