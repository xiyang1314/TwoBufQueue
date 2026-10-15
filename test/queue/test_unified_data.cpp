#include "../support/writer_fixture.hpp"
#include "../support/warm_pool_test_support.hpp"
#include "TwoBufQueue/src/read/read_layer1/read_service.hpp"
#include "../func/support.hpp"
#include "TwoBufQueue/src/write/writer.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <type_traits>
using namespace TwoBufQueue;
struct Receiver {
    bool reject = false;
    std::size_t calls = 0, messages = 0;
    std::vector<unsigned> values;
    void begin_read_complete(std::int64_t) noexcept {}
    bool read_complete(SpscWriteBlock* block) noexcept {
        const auto bytes = block->readable();
        return read_current(block, bytes.data(), bytes.size());
    }
    void end_read_complete() noexcept {}
    void begin_read_current(std::int64_t) noexcept {}
    bool read_current(SpscWriteBlock*, const std::byte* p, std::size_t length) noexcept {
        ++calls;
        if (reject) return false;
        const auto* end = p + length;
        while (p < end) {
            assert(end - p >= 2);
            const auto size = std::to_integer<unsigned>(p[0]) | (std::to_integer<unsigned>(p[1]) << 8);
            p += 2;
            assert(static_cast<std::size_t>(end - p) >= size);
            if (size == sizeof(unsigned)) {
                unsigned value; std::memcpy(&value, p, sizeof(value)); values.push_back(value);
            }
            ++messages; p += size;
        }
        return true;
    }
    void end_read() noexcept {}
};
template<class TCallback> static void tick(ReadService<TCallback>& service) {
    static const std::atomic<bool> cancel{false};
    const auto result = service.read_once(true, cancel);
    assert(result.error == ReadError::none);
}

static void direct_slots_and_lifetime() {
    WriterFixture buffer(6 * 1500, 1, 1, {}, 4);
    Writer<1, true> writer(&buffer.queue, 0, buffer.backpressure);
    Receiver receiver;
    AutoResetEvent event; ReadService service({}, receiver, event);
    assert(service.add_writer(&writer, false)); tick(service);
    for (unsigned i = 0; i < 1500; ++i) {
        assert(writer.write(i)); tick(service); tick(service);
    }
    assert(receiver.messages == 1500 && writer.source_block()->read_pos() == 9000);
    assert(!writer.write(1500u)); // Completed record must be consumed before a one-block pool can reuse it.
    tick(service);
    assert(buffer.handoff_queue().empty() && buffer.all_released());
    assert(writer.write(1500u)); writer.finish(); tick(service);
    assert(receiver.values.back() == 1500u && buffer.all_released());
    assert(!writer.write(1501u));
    assert(service.stats().saved_data_bytes == 1501 * 6);
}

static void rejection_and_empty() {
    WriterFixture buffer(12, 1, 1); Writer<> writer(&buffer.queue, 0, buffer.backpressure);
    Receiver receiver; receiver.reject = true;
    AutoResetEvent event; ReadService service({}, receiver, event);
    assert(service.add_writer(&writer, false)); tick(service);
    assert(writer.write(7u)); tick(service); tick(service);
    auto* source = writer.source_block();
    assert(source->read_pos() == 0);
    for (int i = 0; i < 2000; ++i) tick(service);
    assert(source->read_pos() == 0 && source->readable().size() == 6);
    assert(writer.write(8u)); writer.finish();
    receiver.reject = false;
    for (int i = 0; i < 4; ++i) tick(service);
    assert((receiver.values == std::vector<unsigned>{7,8}));
    assert(buffer.all_released());

    WriterFixture zero(8,1,1); Writer<> zw(&zero.queue, 1, zero.backpressure); Receiver zr;
    AutoResetEvent ze; ReadService zs({},zr,ze);
    assert(zs.add_writer(&zw, false));
    assert(zw.write()); assert(zw.write_mem(nullptr,0)); zw.finish(); tick(zs);
    assert(zr.messages == 2 && zs.stats().saved_data_bytes == 4 && zero.all_released());
}

static void buffered_partial_backpressure() {
    ft::Buffered<> pipeline(6, 4, 4, 6, 1, 1, 1, 1);
    for (unsigned i = 0; i < 4; ++i) assert(pipeline.writer.write(i));
    pipeline.writer.finish();
    tick(pipeline.service);
    // A received loan pins the only output block. Copying must retry without loss.
    auto held = pipeline.output.try_receive();
    assert(held && pipeline.source.in_use_count() != 0);
    assert(to_data_batch(*held).data_size == 6);
    tick(pipeline.service);
    assert(!pipeline.output.try_receive());
    assert(pipeline.output.finish(held));
    std::vector<unsigned> values{0};
    for (unsigned i = 1; i < 4; ++i) {
        tick(pipeline.service);
        auto loan = pipeline.output.try_receive();
        assert(loan);
        unsigned value;
        std::memcpy(&value, loan->frames()[0].data + 2, sizeof(value));
        values.push_back(value);
        assert(pipeline.output.finish(loan));
    }
    tick(pipeline.service);
    assert((values == std::vector<unsigned>{0,1,2,3}));
    assert(pipeline.source.all_released() && pipeline.pool.all_released());
    assert(pipeline.saver.error_code() == ToReadBlockError::none);
}

static void limits() {
    WriterFixture buffer(65537,1,1,{},4); Writer<1, true> writer(&buffer.queue, 0, buffer.backpressure);
    std::vector<std::byte> payload(65536);
    assert(writer.write_mem(payload.data(),65535));
    Receiver receiver; AutoResetEvent event; ReadService service({},receiver,event);
    assert(service.add_writer(&writer, false)); writer.finish(); tick(service);
    assert(receiver.messages == 1 && buffer.all_released());
}
static void inherited_pool_ownership() {
    WriterFixture pool(48, 1, 1, {}, 4), other(48, 1, 1, {}, 4);
    SpscWriteBlock* previous = nullptr;
    for (unsigned round = 0; round < 100; ++round) {
        Writer<1, true> writer(&pool.queue, 0, pool.backpressure);
        auto& queue = writer.handoff_queue();
        auto start = queue.try_claim();
        assert(start && !start.get()); queue.finish(start);
        auto current = queue.current();
        auto* block = writer.source_block();
        assert(current.get() == block && (!previous || previous == block));
        assert(queue.owns(current) && !other.handoff_queue().owns(current));
        assert(writer.write(42u));
        writer.finish();
        assert(!queue.current_producer());
        auto claim = queue.try_claim();
        assert(claim.get() == block && !queue.callbacks().can_finish(*block));
        unsigned value;
        std::memcpy(&value, claim->readable().data() + 2, sizeof(value));
        assert(value == 42 && claim->consume(claim->readable().size()));
        queue.finish(claim);
        assert(pool.all_released());
        previous = block;
    }
}
int main() {
    inherited_pool_ownership();
    direct_slots_and_lifetime(); rejection_and_empty(); buffered_partial_backpressure(); limits();
    std::cout << "PASS unified frames: 1500 current reads, immediate source recycle, rejection, copied queue, prefixes and limits\n";
}
