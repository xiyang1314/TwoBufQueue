#include "writer_fixture.hpp"
#include "TwoBufQueue/src/write/writer.hpp"
#include <cassert>
#include <vector>
#include <iostream>
#include <limits>

using namespace TwoBufQueue;

static void drain(WriterFixture& mgr, std::vector<std::byte>& output) {
    auto& queue = mgr.handoff_queue();
    while (auto claim = queue.try_pop()) {
        if (!claim.get()) { queue.finish(claim); continue; }
        const auto bytes = claim->readable();
        output.insert(output.end(), bytes.begin(), bytes.end());
        assert(claim->consume(bytes.size()));
        queue.finish(claim);
    }
}
template<class T> static void append(std::vector<std::byte>& bytes, const T& value) {
    auto* data = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(value));
}

template<unsigned Count> static void mixed_payloads() {
    WriterFixture mgr(64, 2, 2);
    std::vector<std::byte> expected, actual;
    const std::uint8_t a = 7;
    const double b = 3.25;
    const std::int16_t c = -123;
    const unsigned char raw[] = {0, 255, 42};
    {
        Writer<Count> writer(&mgr.queue, 0, mgr.backpressure);
        auto* begin = writer.published_write_ptr();
        assert(writer.write(a, b, c));
        append(expected, std::uint16_t{11});
        append(expected, a); append(expected, b); append(expected, c);
        if constexpr (Count == 1) assert(writer.published_write_ptr() == begin + expected.size());
        else assert(writer.published_write_ptr() == begin);
        assert(!writer.write_mem(nullptr, 1));
        assert(!writer.write_mem(raw, (std::numeric_limits<std::uint16_t>::max)()));
        assert(writer.write_mem(nullptr, 0)); append(expected, std::uint16_t{0});
        assert(writer.write()); append(expected, std::uint16_t{0});
        assert(writer.write_mem(raw, sizeof(raw)));
        append(expected, std::uint16_t{3});
        for (auto value : raw) expected.push_back(static_cast<std::byte>(value));
        // Empty messages contain a prefix and count toward publication; failed calls do not.
        if constexpr (Count == 2) assert(writer.published_write_ptr() == begin + expected.size());
        if constexpr (Count == 3) assert(writer.published_write_ptr() == begin + 17);
        writer.flush();
        assert(writer.published_write_ptr() == begin + expected.size());
        assert(std::memcmp(begin, expected.data(), expected.size()) == 0);
        assert(writer.write(std::uint32_t{99}));
        append(expected, std::uint16_t{4});
        append(expected, std::uint32_t{99}); // Destructor publishes the remaining tail.
    }
    drain(mgr, actual);
    assert(actual == expected);
    assert(mgr.all_released());
}

template<unsigned Count> static void rollover() {
    WriterFixture mgr(64, 2, 2);
    std::vector<std::byte> actual;
    unsigned char raw[60]{};
    raw[59] = 77;
    {
        Writer<Count> writer(&mgr.queue, 0, mgr.backpressure);
        assert(writer.write_mem(raw, sizeof(raw)));
        assert(writer.write(std::uint64_t{123}));
    }
    drain(mgr, actual);
    assert(actual.size() == 72);
    assert(std::memcmp(actual.data()+2, raw, 60) == 0);
    std::uint64_t tail{};
    std::memcpy(&tail, actual.data() + 64, sizeof(tail));
    assert(tail == 123);
}

template<unsigned Count> static void backpressure_retry() {
    WriterFixture mgr(64, 1, 1);
    std::vector<std::byte> actual;
    unsigned char raw[62]{};
    {
        Writer<Count> writer(&mgr.queue, 0, mgr.backpressure);
        assert(writer.write_mem(raw, sizeof(raw)));
        assert(!writer.write(std::uint32_t{9}));
        assert(writer.source_block() == nullptr);
        drain(mgr, actual);
        assert(actual.size() == 64);
        assert(writer.write(std::uint32_t{9}));
        assert(writer.backpressure_stats().dropped_records == 1);
    }
    drain(mgr, actual);
    assert(actual.size() == 70);
    std::uint32_t tail{};
    std::memcpy(&tail, actual.data() + 66, sizeof(tail));
    assert(tail == 9);
}

static void pressure_query() {
    WriterFixture mgr(64, 1, 3);
    std::vector<std::byte> actual;
    unsigned char raw[62]{};
    assert(mgr.pool.in_use_count() == 0);
    {
        Writer<> writer(&mgr.queue, 0, mgr.backpressure);
        assert(mgr.block_count() == 1);
        // One of three permitted blocks, not one of one allocated blocks.
        assert(writer.usage_at_or_above(33));
        assert(!writer.usage_at_or_above(34));
        assert(!writer.usage_at_or_above(0));
        assert(!writer.usage_at_or_above(100));
        auto* begin = writer.published_write_ptr();
        assert(writer.usage_at_or_above(1));
        assert(writer.published_write_ptr() == begin);
        assert(writer.backpressure_stats().dropped_records == 0);
        assert(writer.write_mem(raw, sizeof(raw)));
        assert(writer.write_mem(raw, sizeof(raw)));
        assert(writer.usage_at_or_above(66));
        assert(!writer.usage_at_or_above(67));
        // Query does not impose a drop policy: writing can fill the third block.
        assert(writer.write_mem(raw, sizeof(raw)));
        assert(writer.usage_at_or_above(100));
        assert(writer.usage_at_or_above(255));
        drain(mgr, actual);
        // Return completed old blocks immediately; only the currently written block stays occupied.
        assert(!writer.usage_at_or_above(100) && mgr.in_use_count() == 1);
        assert(writer.backpressure_stats().dropped_records == 0);
    }
    drain(mgr, actual);
    assert(actual.size() == 192);
    assert(mgr.all_released() && mgr.in_use_count() == 0);
}

static void finish_block_and_continue() {
    constexpr auto block_only = (std::numeric_limits<std::uint32_t>::max)();
    WriterFixture source(64, 1, 1);
    std::vector<std::byte> actual, expected;
    Writer<block_only> writer(&source.queue, 0);
    for (unsigned i = 0; i != 20; ++i) {
        assert(writer.write(i));
        append(expected, std::uint16_t{sizeof(i)}); append(expected, i);
        writer.finish_block(); writer.finish_block(); // Idempotent; does not finish Writer.
        assert(writer.source_block() == nullptr);
        drain(source, actual);
        assert(actual == expected && source.all_released());
    }
    writer.finish();
    assert(!writer.write(21u));
}

// Writing an object must bypass a user-defined operator&.
struct AddressTrap {
    std::uint32_t value;
    const AddressTrap* operator&() const noexcept;
};
static const AddressTrap decoy{0x55667788};
const AddressTrap* AddressTrap::operator&() const noexcept { return std::addressof(decoy); }
static_assert(std::is_trivially_copyable_v<AddressTrap>);
static void true_argument_addresses() {
    WriterFixture source(64, 1, 1);
    std::vector<std::byte> actual, expected;
    const AddressTrap first{0x11223344}, second{0x778899aa};
    {
        Writer<> writer(&source.queue, 0);
        assert(writer.write(first));
        append(expected, std::uint16_t{4}); append(expected, first.value);
        assert(writer.write(first, second));
        append(expected, std::uint16_t{8}); append(expected, first.value); append(expected, second.value);
        assert(writer.write_mem(std::addressof(second), sizeof(second)));
        append(expected, std::uint16_t{4}); append(expected, second.value);
    }
    drain(source, actual);
    assert(actual == expected && source.all_released());
}

int main() {
    constexpr auto block_only = (std::numeric_limits<std::uint32_t>::max)();
    true_argument_addresses();
    pressure_query();
    finish_block_and_continue();
    mixed_payloads<1>(); mixed_payloads<2>(); mixed_payloads<3>(); mixed_payloads<block_only>();
    rollover<1>(); rollover<3>(); rollover<block_only>();
    backpressure_retry<2>(); backpressure_retry<block_only>();
    std::cout << "writer payload, publication, rollover and backpressure tests passed\n";
}
