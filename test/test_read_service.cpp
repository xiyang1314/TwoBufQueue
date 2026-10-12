#include "read_test_support.hpp"
using namespace read_test;

static void empty_records() {
    Rig r;
    auto result = r.tick();
    CHECK(result.progressed && !result.pending);
    r.probe.expect({{'e'}});
    CHECK(r.source.queue.empty() && r.service.stats().saved_finished_ranges == 0);
    r.probe.events.clear(); r.writer.finish(); result = r.tick();
    r.probe.expect({{'e'}});
    CHECK(result.progressed && !result.pending && r.source.all_released());
    CHECK(r.service.stats().saved_finished_ranges == 1 && r.service.stats().saved_frames == 0);
}
static void completed_priority() {
    Rig r(6); WriterFixture source(6, 4, 4); Writer<> writer(&source.queue, 1);
    CHECK(r.service.add_queue(&source.queue, 1));
    CHECK(r.writer.write(0u) && r.writer.write(1u));
    CHECK(writer.write(10u) && writer.write(11u));
    const auto result = r.tick();
    CHECK(result.progressed && !result.pending);
    r.probe.expect({{'B'}, {'F',0,6}, {'F',1,6}, {'E'}, {'b'}, {'C',0,6}, {'C',1,6}, {'e'}});
    CHECK(r.probe.values(0) == std::vector<unsigned>({0,1}));
    CHECK(r.probe.values(1) == std::vector<unsigned>({10,11}));
    CHECK(r.service.stats().saved_frames == 4 && r.service.stats().saved_partial_frames == 2);
    writer.finish(); r.drain(); CHECK(source.all_released());
}
static void completed_rejection() {
    Rig r(6); WriterFixture source(6, 4, 4); Writer<> writer(&source.queue, 1);
    CHECK(r.service.add_writer(&writer));
    CHECK(r.writer.write(0u) && r.writer.write(1u) && r.writer.write(2u));
    CHECK(writer.write(10u) && writer.write(11u));
    r.probe.decide = [](char, SpscWriteBlock*, const std::byte* ptr, std::size_t) { return value_at(ptr) != 1; };
    auto result = r.tick();
    CHECK(result.pending && result.progressed && result.error == ReadError::none);
    r.probe.expect({{'B'}, {'F',0,6}, {'F',0,6}, {'E'}, {'e'}});
    CHECK(r.probe.values(0) == std::vector<unsigned>{0} && r.probe.values(1).empty());
    CHECK(r.source.queue.empty() && r.service.pending_finished_count() == 3);
    CHECK(source.queue.pending_count() == 2 && r.service.stats().saved_finished_ranges == 1);
    r.probe.events.clear(); result = r.tick();
    CHECK(!result.progressed && result.pending);
    r.probe.expect({{'B'}, {'F',0,6}, {'E'}, {'e'}});
    CHECK(r.service.stats().memory_full_events == 2 && r.service.stats().memory_backpressured_frames == 1);
    // New records may enter the freed queue slots while the failed block is held by Service.
    CHECK(r.writer.write(3u));
    CHECK(r.source.queue.pending_count() == 1 && r.service.pending_finished_count() == 4);
    r.probe.decide = {}; (void)r.tick();
    r.probe.verify(0, 4); CHECK(r.probe.values(1) == std::vector<unsigned>({10,11}));
    writer.finish(); r.drain(); CHECK(source.all_released());
}
static void current_rejection() {
    Rig r; WriterFixture source(24, 2, 2); Writer<> writer(&source.queue, 1);
    CHECK(r.service.add_writer(&writer));
    CHECK(r.writer.write(0u) && writer.write(10u));
    int rejected = 0;
    r.probe.decide = [&](char, SpscWriteBlock* block, const std::byte*, std::size_t) {
        return block->source_id() != rejected;
    };
    CHECK(r.tick().pending);
    r.probe.expect({{'b'}, {'C',0,6}, {'e'}});
    CHECK(r.writer.source_block()->read_pos() == 0 && writer.source_block()->read_pos() == 0);
    rejected = 1; r.probe.events.clear(); CHECK(r.tick().pending);
    r.probe.expect({{'b'}, {'C',0,6}, {'C',1,6}, {'e'}});
    CHECK(r.writer.source_block()->read_pos() == 6 && writer.source_block()->read_pos() == 0);
    // No priority retry for the previously rejected writer: registration order remains authoritative.
    CHECK(r.writer.write(1u)); rejected = -1; r.probe.events.clear();
    CHECK(!r.tick().pending);
    r.probe.expect({{'b'}, {'C',0,6}, {'C',1,6}, {'e'}});
    r.probe.verify(0,2); CHECK(r.probe.values(1) == std::vector<unsigned>{10});
    writer.finish(); r.drain(); CHECK(source.all_released());
}
static void retry_reclassified() {
    Rig r;
    CHECK(r.writer.write(0u)); r.probe.accept = false;
    CHECK(r.tick().pending && r.writer.source_block()->read_pos() == 0);
    CHECK(r.writer.write(1u)); r.writer.finish();
    r.probe.accept = true; r.probe.events.clear();
    CHECK(!r.tick().pending);
    r.probe.expect({{'B'}, {'F',0,12}, {'E'}, {'e'}});
    r.probe.verify(0,2); CHECK(r.source.all_released());
}
static void fixed_prefix() {
    Rig r(6); WriterFixture other(6, 4, 4); Writer<> writer(&other.queue, 1);
    CHECK(r.service.add_writer(&writer));
    CHECK(r.writer.write(0u) && r.writer.write(1u) && writer.write(10u));
    auto* saved_current = r.writer.source_block();
    auto* other_current = writer.source_block();
    r.probe.begin_complete_hook = [&] {
        // Both queues were snapshotted before this hook. These records belong to the next round.
        CHECK(r.writer.write(2u) && writer.write(11u));
    };
    CHECK(r.tick().pending);
    r.probe.expect({{'B'}, {'F',0,6}, {'E'}, {'b'}, {'C',0,6}, {'C',1,6}, {'e'}});
    CHECK(saved_current->read_pos() == 6 && other_current->read_pos() == 6);
    CHECK(r.source.queue.pending_count() == 1 && other.queue.pending_count() == 1);
    CHECK(r.service.stats().saved_finished_ranges == 1);
    r.probe.begin_complete_hook = {}; r.probe.events.clear(); CHECK(!r.tick().pending);
    r.probe.expect({{'b'}, {'C',0,6}, {'C',1,6}, {'e'}}); // empty finished tails don't begin a phase
    CHECK(r.service.stats().saved_finished_ranges == 3);
    r.probe.verify(0,3); CHECK(r.probe.values(1) == std::vector<unsigned>({10,11}));
    writer.finish(); r.drain(); CHECK(other.all_released());
}
static void current_snapshot() {
    Rig r;
    CHECK(r.writer.write(0u));
    r.probe.decide = [&](char phase, SpscWriteBlock*, const std::byte*, std::size_t size) {
        CHECK(phase == 'C' && size == 6); CHECK(r.writer.write(1u)); return true;
    };
    (void)r.tick();
    CHECK(r.writer.source_block()->read_pos() == 6 && r.writer.source_block()->readable().size() == 6);
    CHECK(r.probe.values(0) == std::vector<unsigned>{0});
    r.probe.decide = {}; (void)r.tick(); r.probe.verify(0,2); r.drain();
}
static void scan_timing() {
    auto cfg = config(); cfg.idle_rescan_interval = cfg.idle_rescan_max_interval = 1h;
    Rig r(24, 4, cfg);
    CHECK(r.writer.write(0u)); r.probe.accept = false;
    CHECK(r.tick(false).pending);
    CHECK(r.service.next_scan_deadline() > 30min); // failed callback still updates the schedule
    auto calls = r.probe.current_calls.load();
    CHECK(r.tick(false).pending && r.probe.current_calls == calls);
    CHECK(r.writer.source_block()->read_pos() == 0);
    r.probe.accept = true; CHECK(!r.tick(true).pending); // forced scan bypasses the deadline
    CHECK(r.writer.write(1u)); calls = r.probe.current_calls.load();
    CHECK(r.tick(false).pending && r.probe.current_calls == calls); // success also schedules
    r.writer.finish(); CHECK(!r.tick(false).pending); // completed records ignore current's deadline
    r.probe.verify(0,2); CHECK(r.source.all_released());
    (void)r.tick(true); CHECK(r.service.next_scan_deadline() > 30min); // null current is an empty scan
}
static void cancelled_or_expired() {
    Rig r; CHECK(r.writer.write(0u)); r.writer.finish();
    const auto count = r.source.queue.pending_count();
    r.cancel = true;
    CHECK(r.tick().pending); r.probe.expect({{'e'}});
    r.cancel = false; r.probe.events.clear();
    CHECK(r.service.read_once(true, r.cancel, AutoResetEvent::Clock::now() - 1ms).pending);
    r.probe.expect({{'e'}}); CHECK(r.source.queue.pending_count() == count);
    r.probe.events.clear(); r.probe.begin_complete_hook = [&] { r.cancel = true; };
    CHECK(r.tick().pending); r.probe.expect({{'B'}, {'E'}, {'e'}});
    CHECK(r.source.queue.empty() && r.service.pending_finished_count() == 1);
    r.drain(); r.probe.verify(0,1);
}
static void begin_current_stop() {
    Rig r; CHECK(r.writer.write(0u));
    r.probe.begin_current_hook = [&] { r.cancel = true; };
    CHECK(r.tick().pending); r.probe.expect({{'b'}, {'e'}});
    CHECK(r.writer.source_block()->read_pos() == 0);
    r.cancel = false; r.probe.events.clear();
    const auto deadline = AutoResetEvent::Clock::now() + 100ms;
    r.probe.begin_current_hook = [&] { CHECK(wait_until([&] { return AutoResetEvent::Clock::now() >= deadline; })); };
    CHECK(r.service.read_once(true, r.cancel, deadline).pending);
    r.probe.expect({{'b'}, {'e'}}); CHECK(r.writer.source_block()->read_pos() == 0);
    r.drain(); r.probe.verify(0,1);
}
static void stop_after_success() {
    Rig r(6); CHECK(r.writer.write(0u) && r.writer.write(1u) && r.writer.write(2u));
    r.probe.decide = [&](char, SpscWriteBlock*, const std::byte*, std::size_t) { r.cancel = true; return true; };
    CHECK(r.tick().pending);
    r.probe.expect({{'B'}, {'F',0,6}, {'E'}, {'e'}});
    CHECK(r.service.stats().saved_finished_ranges == 1 && r.source.queue.pending_count() == 1);
    r.drain(); r.probe.verify(0,3);
}
static void registration() {
    Rig r; WriterFixture other(24,1,1); Writer<> writer(&other.queue,1);
    CHECK(!r.service.add_writer(nullptr) && r.service.error_code() == ReadError::null_dependency);
    CHECK(!r.service.add_queue(&r.source.queue, 2) && r.service.error_code() == ReadError::duplicate_writer);
    CHECK(!r.service.add_queue(&other.queue, 0) && r.service.error_code() == ReadError::duplicate_writer);
    CHECK(!r.service.add_queue(&other.queue, -1) && r.service.error_code() == ReadError::invalid_registration);
    CHECK(!r.service.add_writer(&writer, true));
    r.service.prepare_start();
    CHECK(r.source.queue.has_read_available_callback() && !r.service.add_writer(&writer));
    r.service.detach_completed_callbacks(); CHECK(!r.source.queue.has_read_available_callback());
    CHECK(r.service.add_queue(&other.queue, 1)); CHECK(r.service.writer_count() == 2);
    CHECK(r.service.stats().error_count == 6);
    writer.finish(); r.drain(); CHECK(other.all_released());
}
static void writer_lifetime() {
    WriterFixture source(24,1,1); Probe probe; AutoResetEvent event;
    ReadService service(config(), probe, event);
    { Writer<> writer(&source.queue,7); CHECK(service.add_writer(&writer)); CHECK(writer.write(0u)); }
    const std::atomic<bool> cancel{false};
    CHECK(!service.read_once(true,cancel).pending);
    probe.verify(7,1); CHECK(source.all_released());
}
static void generation_stats() {
    Rig r(12,1);
    CHECK(r.writer.write(0u)); auto* first = r.writer.source_block(); const auto sequence = first->sequence_;
    r.probe.accept = false; (void)r.tick(); (void)r.tick();
    CHECK(r.service.stats().memory_full_events == 2 && r.service.stats().memory_backpressured_frames == 1);
    r.probe.accept = true; (void)r.tick(); CHECK(r.writer.write(1u));
    r.probe.accept = false; (void)r.tick();
    CHECK(r.service.stats().memory_backpressured_frames == 2);
    r.drain();
    Writer<> writer(&r.source.queue, 0);
    CHECK(writer.source_block() == first && writer.source_block()->sequence_ != sequence);
    CHECK(writer.write(2u)); r.probe.accept = false; (void)r.tick();
    CHECK(r.service.stats().memory_backpressured_frames == 3);
    r.probe.accept = true; writer.finish(); CHECK(!r.tick().pending);
    r.probe.verify(0,3); CHECK(r.service.stats().saved_data_bytes == 18 && r.source.all_released());
}
int main(int argc, char** argv) {
    return run_cases(argc, argv, {{"empty_records",empty_records}, {"completed_priority",completed_priority},
        {"completed_rejection",completed_rejection}, {"current_rejection",current_rejection},
        {"retry_reclassified",retry_reclassified}, {"fixed_prefix",fixed_prefix},
        {"current_snapshot",current_snapshot}, {"scan_timing",scan_timing},
        {"cancelled_or_expired",cancelled_or_expired}, {"begin_current_stop",begin_current_stop},
        {"stop_after_success",stop_after_success}, {"registration",registration},
        {"writer_lifetime",writer_lifetime}, {"generation_stats",generation_stats}});
}
