#include "TwoBufQueue/src/write/writer.hpp"
#include "TwoBufQueue/src/read/read_layer1/read_service.hpp"
#include <cassert>
#include <cstring>

using namespace TwoBufQueue;

template<class TFunction> void invalid(TFunction&& action) {
    bool caught = false;
    try { action(); } catch (const std::invalid_argument&) { caught = true; }
    assert(caught);
}
static void drain(WriterQueue& queue) {
    while (auto claim = queue.try_pop()) {
        if (claim.get()) assert(claim->consume(claim->readable().size()));
        queue.finish(claim);
    }
}
// These scenarios expect a resource; acknowledge any start records explicitly.
static WriterQueue::Record completed(WriterQueue& queue) {
    for (;;) {
        auto claim = queue.try_pop();
        assert(claim);
        if (claim.get()) return claim;
        queue.finish(claim);
    }
}
static void construction() {
    WriterPool pool(0, 2, SpscBlockFactory<SpscWriteBlock>{8});
    auto queue = make_writer_queue(pool, 4);
    invalid([] { Writer<> writer(nullptr, 0); });
    invalid([&] { Writer<> writer(&queue, 0, {.wait_get_timeout = std::chrono::milliseconds{0}}); });
    {
        WriterQueue bare(pool, 4);
        invalid([&] { Writer<> writer(&bare, 0); });
    }
    assert(pool.all_released());
    {
        Writer<> first(&queue, 0);
        invalid([&] { Writer<> second(&queue, 1); });
        assert(first.write(11u));
    }
    drain(queue);
    // No message-layout metadata in the pool: a sequential raw writer can reuse it.
    {
        Writer<1, false> raw(&queue, 0);
        assert(raw.write(std::uint64_t{42}));
    }
    auto claim = completed(queue);
    std::uint64_t value;
    std::memcpy(&value, claim->readable().data(), sizeof(value));
    assert(value == 42 && claim->consume(sizeof(value)));
    queue.finish(claim);
    queue.close();
    invalid([&] { Writer<> writer(&queue, 0); });
    assert(queue.is_drained() && pool.all_released());
}
static void queue_pressure() {
    WriterPool pool(0, 4, SpscBlockFactory<SpscWriteBlock>{6});
    auto queue = make_writer_queue(pool, 2, true);
    Writer<> writer(&queue, 0);
    assert(writer.write(1u) && !writer.write(2u));
    assert(queue.stats().queue_full == 1 && queue.stats().pool_limit == 0);
    assert(writer.backpressure_stats().pressure_events == 1 && writer.backpressure_stats().dropped_records == 1);
    auto claim = completed(queue);
    unsigned value;
    std::memcpy(&value, claim->readable().data()+2, sizeof(value));
    assert(value == 1 && claim->consume(6));
    queue.finish(claim);
    assert(writer.write(2u)); writer.finish();
    claim = completed(queue);
    std::memcpy(&value, claim->readable().data()+2, sizeof(value));
    assert(value == 2 && claim->consume(6));
    queue.finish(claim);
    queue.close();
    assert(queue.is_drained() && pool.all_released());
}
static void independent_lifetime() {
    WriterPool pool(1, 1, SpscBlockFactory<SpscWriteBlock>{64});
    auto queue = make_writer_queue(pool);
    struct Callback {
        unsigned received = 0;
        void begin_read_complete(std::int64_t) noexcept {}
        bool read_complete(SpscWriteBlock* block) noexcept {
            const auto bytes = block->readable();
            return read_current(block, bytes.data(), bytes.size());
        }
        void end_read_complete() noexcept {}
        void begin_read_current(std::int64_t) noexcept {}
        bool read_current(SpscWriteBlock* block, const std::byte* ptr, std::size_t size) noexcept {
            assert(size == 6 && block->source_id() == 9);
            std::memcpy(&received, ptr + 2, sizeof(received)); return true;
        }
        void end_read() noexcept {}
    } callback;
    AutoResetEvent event;
    ReadService reader({}, callback, event);
    {
        Writer<> writer(&queue, 9);
        assert(reader.add_writer(&writer));
        assert(writer.write(123u));
    } // The reader retains only the external queue, so Writer may already be gone.
    const std::atomic<bool> cancel{false};
    assert(!reader.read_once(true, cancel).pending);
    assert(callback.received == 123 && pool.all_released());
    queue.close();
    assert(queue.is_drained());
}
static void closed_handoff() {
    WriterPool pool(1, 2, SpscBlockFactory<SpscWriteBlock>{6});
    auto queue = make_writer_queue(pool);
    Writer<> writer(&queue, 0);
    assert(writer.write(42u));
    queue.close();
    assert(!writer.write(43u)); // Final old block is still delivered; no new resource is acquired.
    writer.finish();
    auto claim = completed(queue);
    unsigned value;
    std::memcpy(&value, claim->readable().data()+2, sizeof(value));
    assert(value == 42 && claim->consume(6));
    queue.finish(claim);
    assert(queue.is_drained() && pool.all_released());
}
int main() { construction(); queue_pressure(); independent_lifetime(); closed_handoff(); }
