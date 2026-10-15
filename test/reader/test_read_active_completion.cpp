#include "../support/read_test_support.hpp"
using namespace read_test;

template<bool TAccept> static void completion_during_current() {
    for (unsigned round = 0; round != 30; ++round) {
        Rig r(12,4); CHECK(r.writer.write(0u));
        auto* original = r.writer.source_block();
        Gate gate;
        r.probe.decide = [&](char phase, SpscWriteBlock* block, const std::byte*, std::size_t size) {
            CHECK(phase == 'C' && block == original && size == 6);
            gate.pause(); return TAccept;
        };
        ReadResult result;
        std::thread reader([&] { result = r.tick(); });
        gate.wait();
        CHECK(r.writer.write(1u) && r.writer.write(2u) && r.writer.write(3u));
        r.writer.finish();
        CHECK(original->state() == SpscWriteBlockState::WriteFinished && r.source.in_use_count() == 2);
        gate.release(); reader.join();
        CHECK(original->read_pos() == (TAccept ? 6u : 0u));
        CHECK(result.pending && !original->is_free());
        r.probe.decide = {}; r.drain(); r.probe.verify(0,4);
        CHECK(r.service.stats().saved_data_bytes == 24);
    }
}
static void accepted_current() { completion_during_current<true>(); }
static void rejected_current() { completion_during_current<false>(); }
static void prefix_append() {
    for (unsigned round = 0; round != 30; ++round) {
        Rig r(6,4); CHECK(r.writer.write(0u) && r.writer.write(1u));
        auto* saved_current = r.writer.source_block();
        Gate gate; r.probe.begin_complete_hook = [&] { gate.pause(); };
        ReadResult result;
        std::thread reader([&] { result = r.tick(); });
        gate.wait();
        CHECK(r.writer.write(2u)); // publishes {saved_current, next} after the fixed snapshot
        gate.release(); reader.join();
        CHECK(result.pending && saved_current->read_pos() == 6 && !saved_current->is_free());
        CHECK(r.source.queue.pending_count() == 1 && r.service.stats().saved_finished_ranges == 1);
        r.probe.expect({{'B'}, {'F',0,6}, {'E'}, {'b'}, {'C',0,6}, {'e'}});
        r.drain(); r.probe.verify(0,3);
    }
}
static void single_pool_reacquire() {
    Rig r(6,1); CHECK(r.writer.write(0u));
    auto* address = r.writer.source_block(); const auto generation = address->sequence_;
    CHECK(!r.writer.write(1u) && !r.writer.source_block()); // {finished,null}
    CHECK(!r.tick().pending && r.source.all_released());
    CHECK(r.writer.write(1u)); // {null,next}; same address, a new generation
    CHECK(r.writer.source_block() == address && address->sequence_ != generation);
    CHECK(!r.tick().pending && address->read_pos() == 6);
    r.drain(); r.probe.verify(0,2);
    CHECK(r.service.stats().saved_finished_ranges == 2 && r.service.stats().saved_data_bytes == 12);
}
int main(int argc, char** argv) {
    return run_cases(argc, argv, {{"accepted_current",accepted_current}, {"rejected_current",rejected_current},
        {"prefix_append",prefix_append}, {"single_pool_reacquire",single_pool_reacquire}});
}
