#include "../support/writer_fixture.hpp"
#include "TwoBufQueue/src/write/writer.hpp"
#include "TwoBufQueue/src/write/seq_writer.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <cassert>
#include <cstring>
#include <iostream>
#include <type_traits>
#include <utility>
#include <vector>
using namespace TwoBufQueue;
using namespace std::chrono_literals;
using Plain = Writer<UINT32_MAX, false>;
using Framed = Writer<UINT32_MAX, true, std::uint16_t, void>;
using Sequenced = SeqWriter<std::uint32_t, UINT32_MAX, true>;
using Result = SessionWriteResult;
template<class TSlice> concept HasClose = requires(TSlice& session) { session.close(); };
static_assert(!HasClose<Plain::WriteSession> && !HasClose<Framed::WriteSession> && !HasClose<Sequenced::WriteSession>);
static_assert(std::is_nothrow_default_constructible_v<Plain::WriteSession>);

template<class TWriter> concept HasSession = requires(TWriter& writer) {
    writer.get_current_session(); writer.get_next_session(1ms);
};
static_assert(!HasSession<Writer<>> && !HasSession<Writer<32, false>>);
static_assert(HasSession<Plain> && HasSession<Framed> && HasSession<Sequenced>);
template<class TWriter> constexpr bool movable_session =
    std::is_nothrow_move_constructible_v<typename TWriter::WriteSession> &&
    std::is_nothrow_move_assignable_v<typename TWriter::WriteSession> &&
    !std::is_copy_constructible_v<typename TWriter::WriteSession> &&
    !std::is_copy_assignable_v<typename TWriter::WriteSession>;
static_assert(movable_session<Plain> && movable_session<Framed> && movable_session<Sequenced>);
static void drain(WriterFixture& source, std::vector<std::byte>& actual) {
    while (auto claim = source.queue.try_pop()) {
        if (claim.get()) {
            const auto bytes = claim->readable();
            actual.insert(actual.end(), bytes.begin(), bytes.end());
            assert(claim->consume(bytes.size()));
        }
        source.queue.finish(claim);
    }
}
template<class TValue> static void append(std::vector<std::byte>& bytes, const TValue& value) {
    const auto* data = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(value));
}
static void lifecycle() {
    WriterFixture source(64, 1, 2);
    Plain writer(&source.queue, 0);
    auto* block = writer.source_block();
    auto* begin = writer.published_write_ptr();
    std::vector<std::byte> expected, actual;
    {
        auto hot = writer.get_current_session(); assert(hot);
        auto duplicate = writer.get_current_session(); assert(!duplicate);
        auto duplicate_next = writer.get_next_session(); assert(!duplicate_next);
        assert(writer.source_block() == block);
        assert(duplicate.write(std::uint32_t{1}) == Result::error);
        assert(hot.write(std::uint32_t{9}) == Result::success); append(expected, std::uint32_t{9});
        assert(writer.published_write_ptr() == begin);
        hot.flush(); hot.flush();
        assert(writer.source_block() == block && writer.published_write_ptr() == begin + 4);
        assert(hot.write_mem("abc", 3) == Result::success);
        for (char c : {'a','b','c'}) expected.push_back(static_cast<std::byte>(c));
    }
    // Destruction does not publish the tail; get_current_session reuses the same block/cursor.
    assert(writer.published_write_ptr() == begin + 4);
    {
        auto hot = writer.get_current_session(); assert(hot);
        assert(writer.source_block() == block);
        assert(hot.write(std::uint16_t{12}) == Result::success); append(expected, std::uint16_t{12});
        hot = {}; hot = {}; hot.flush();
        assert(!hot && hot.write(std::uint32_t{1}) == Result::error);
        auto next = writer.get_next_session(); assert(next);
        assert(writer.source_block() != block);
        assert(next.write(std::uint8_t{3}) == Result::success); append(expected, std::uint8_t{3});
        assert(hot.write(std::uint32_t{2}) == Result::error);
    }
    writer.finish(); drain(source, actual);
    assert(actual == expected && source.all_released());
    auto after_finish = writer.get_current_session(1s); assert(!after_finish);
    auto next_after_finish = writer.get_next_session(1s); assert(!next_after_finish);
}
static void bounds_and_single_pool() {
    WriterFixture source(64, 1, 1);
    Plain writer(&source.queue, 0);
    auto* block = writer.source_block();
    std::array<std::byte, 65> raw{};
    for (unsigned i = 0; i != raw.size(); ++i) raw[i] = static_cast<std::byte>(i);
    std::vector<std::byte> actual, expected;
    {
        auto hot = writer.get_current_session();
        assert(hot.write_mem(nullptr, 0) == Result::invalid_argument);
        assert(hot.write_mem(raw.data(), 65) == Result::exceeds_block_capacity);
        assert(hot.write(raw) == Result::exceeds_block_capacity);
        assert(hot.write_mem(raw.data(), 64) == Result::success);
        expected.insert(expected.end(), raw.begin(), raw.begin() + 64);
        assert(hot.write(std::uint32_t{123}) == Result::insufficient_space);
        assert(hot.write(std::uint32_t{123}) == Result::insufficient_space);
        assert(writer.source_block() == block && writer.published_write_ptr() == block->data());
    }
    {
        auto full = writer.get_current_session(); assert(full);
        assert(full.write(std::uint8_t{1}) == Result::insufficient_space);
    }
    // Default Drop may still use a positive acquisition timeout; zero tries once and old is already handed off.
    const auto start = std::chrono::steady_clock::now();
    { auto empty = writer.get_next_session(2ms); assert(!empty); }
    assert(std::chrono::steady_clock::now() - start >= 2ms);
    assert(writer.source_block() == nullptr);
    { auto empty = writer.get_current_session(); assert(!empty); }
    drain(source, actual); assert(actual == expected);
    {
        auto hot = writer.get_current_session(); assert(hot);
        assert(hot.write(std::uint32_t{123}) == Result::success); append(expected, std::uint32_t{123});
    }
    writer.finish(); drain(source, actual);
    assert(actual == expected && source.all_released());
    assert(writer.backpressure_stats().blocked_events == 1);
    assert(writer.backpressure_stats().dropped_records == 0); // Acquisition failure is not a dropped message.
}
static void waiting_overrides_drop() {
    WriterFixture source(64, 1, 1);
    std::atomic<bool> notified{false};
    source.queue.set_read_available_callback([](void* context) noexcept {
        static_cast<std::atomic<bool>*>(context)->store(true, std::memory_order_release);
    }, &notified);
    Plain writer(&source.queue, 0);
    std::vector<std::byte> actual;
    { auto hot = writer.get_current_session(); assert(hot.write(std::array<std::byte,64>{}) == Result::success); }
    std::thread consumer([&] {
        while (!notified.load(std::memory_order_acquire)) std::this_thread::yield();
        std::this_thread::sleep_for(10ms);
        while (actual.size() != 64) { drain(source, actual); std::this_thread::yield(); }
    });
    {
        auto hot = writer.get_next_session(1s); assert(hot);
        assert(hot.write(std::uint32_t{42}) == Result::success);
    }
    consumer.join();
    writer.finish(); drain(source, actual);
    assert(actual.size() == 68 && source.all_released());
    std::uint32_t tail{}; std::memcpy(&tail, actual.data() + 64, 4); assert(tail == 42);
    source.queue.set_read_available_callback(nullptr, nullptr);
}
static void closed_and_invalid_timeout() {
    WriterFixture source(64, 1, 1);
    Plain writer(&source.queue, 0);
    { auto invalid = writer.get_current_session(-1ns); assert(!invalid); }
    std::thread closer([&] { std::this_thread::sleep_for(10ms); source.queue.close(); });
    { auto empty = writer.get_next_session(1s); assert(!empty); }
    closer.join();
    { auto closed = writer.get_current_session(1s); assert(!closed); }
    writer.finish();
}
static void prefixes() {
    WriterFixture a(64, 1, 2), b(64, 1, 2);
    Framed writer(&a.queue, 0); Sequenced seq_writer(&b.queue, 0);
    std::vector<std::byte> actual, expected;
    {
        auto hot = writer.get_current_session();
        assert(hot.write() == Result::success); append(expected, std::uint16_t{0});
        assert(hot.write_mem(nullptr, 0) == Result::invalid_argument);
        assert(hot.write(std::uint8_t{7}, std::uint64_t{99}) == Result::success);
        append(expected, std::uint16_t{9}); append(expected, std::uint8_t{7}); append(expected, std::uint64_t{99});
        assert(hot.write_mem("xy", 2) == Result::success);
        append(expected, std::uint16_t{2}); expected.push_back(std::byte{'x'}); expected.push_back(std::byte{'y'});
    }
    writer.finish(); drain(a, actual); assert(actual == expected);
    actual.clear(); expected.clear();
    {
        auto hot = seq_writer.get_current_session();
        assert(hot.write(0x12345678, std::uint64_t{3}) == Result::success);
        append(expected, std::uint32_t{0x12345678}); append(expected, std::uint16_t{8}); append(expected, std::uint64_t{3});
        assert(hot.write_mem(8, "z", 1) == Result::success);
        append(expected, std::uint32_t{8}); append(expected, std::uint16_t{1}); expected.push_back(std::byte{'z'});
        assert(hot.write(9) == Result::success); append(expected, std::uint32_t{9}); append(expected, std::uint16_t{0});
        assert(hot.write_mem(10, nullptr, 0) == Result::invalid_argument);
        assert(hot.write(std::uint32_t{10}, std::array<std::byte,65>{}) == Result::exceeds_block_capacity);
    }
    seq_writer.finish(); drain(b, actual); assert(actual == expected);
    using Unframed = SeqWriter<std::uint64_t, UINT32_MAX, false>;
    WriterFixture c(64, 1, 2); Unframed u(&c.queue, 0); actual.clear(); expected.clear();
    { auto hot = u.get_current_session();
      assert(hot.write(2, std::uint32_t{99}, std::uint8_t{7}) == Result::success);
      append(expected, std::uint64_t{2}); append(expected, std::uint32_t{99}); append(expected, std::uint8_t{7});
      assert(hot.write_mem(3, "abc", 3) == Result::success); append(expected, std::uint64_t{3});
      for (char x : {'a','b','c'}) expected.push_back(static_cast<std::byte>(x)); }
    u.finish(); drain(c, actual); assert(actual == expected);
}
template<class TWriter> static void move_ownership() {
    WriterFixture a(64, 1, 2), b(64, 1, 2);
    TWriter first_writer(&a.queue, 0), second_writer(&b.queue, 1);
    std::vector<std::byte> expected_a, expected_b, actual_a, actual_b;
    const auto try_write = [](auto& session, std::uint64_t value) {
        if constexpr (std::is_same_v<TWriter, Sequenced>)
            return session.write(static_cast<std::uint32_t>(value), value);
        else return session.write(value);
    };
    const auto write_record = [&](auto& session, auto& expected, std::uint64_t value) {
        assert(try_write(session, value) == Result::success);
        if constexpr (std::is_same_v<TWriter, Sequenced>) append(expected, static_cast<std::uint32_t>(value));
        if constexpr (!std::is_same_v<TWriter, Plain>) append(expected, std::uint16_t{8});
        append(expected, value);
    };
    auto original = first_writer.get_current_session(); assert(original);
    write_record(original, expected_a, 11);
    auto moved = std::move(original);
    assert(!original && moved && try_write(original, 99) == Result::error);
    original = {}; original.flush(); // A moved-from object must not release the destination's exclusive state.
    { auto duplicate = first_writer.get_current_session(); assert(!duplicate); }

    auto destination = second_writer.get_current_session(); assert(destination);
    write_record(destination, expected_b, 22);
    destination = std::move(moved); // Return the second Writer's cursor and adopt the first Writer.
    assert(!moved && destination && try_write(moved, 99) == Result::error);
    moved = {}; moved.flush();
    { auto duplicate = first_writer.get_current_session(); assert(!duplicate); }
    {
        auto second = second_writer.get_current_session(); assert(second);
        write_record(second, expected_b, 33); // Must follow value 22 written before assignment.
    }
    write_record(destination, expected_a, 44);
    auto* same = &destination;
    destination = std::move(*same); // Self-move must preserve the current Session.
    assert(destination);
    assert(first_writer.published_write_ptr() == first_writer.source_block()->data());
    assert(second_writer.published_write_ptr() == second_writer.source_block()->data());

    destination = {};
    destination = first_writer.get_current_session(); assert(destination);
    write_record(destination, expected_a, 66);
    auto empty = first_writer.get_current_session(); assert(!empty);
    auto moved_empty = std::move(empty); assert(!empty && !moved_empty);
    destination = std::move(moved_empty); // Assigning an empty object must still return the existing cursor.
    assert(!destination);
    {
        auto resumed = first_writer.get_current_session(); assert(resumed);
        write_record(resumed, expected_a, 77);
    }
    first_writer.finish(); second_writer.finish();
    drain(a, actual_a); drain(b, actual_b);
    assert(actual_a == expected_a && actual_b == expected_b && a.all_released() && b.all_released());
}
static void double_loop() {
    WriterFixture source(64, 1, 4);
    Framed writer(&source.queue, 0);
    std::vector<std::byte> actual, expected;
    std::uint64_t i = 0;
    auto session = writer.get_current_session(); assert(session);
    while (i != 100) {
        // No background consumer in Fixture; the control path returns previously handed-off blocks first.
        drain(source, actual);
        while (i != 100) {
            const auto status = session.write(i);
            if (status == Result::insufficient_space) break;
            assert(status == Result::success);
            append(expected, std::uint16_t{8}); append(expected, i); ++i;
        }
        if (i == 100) break;
        session = {};
        session = writer.get_next_session(); assert(session);
    }
    session.flush(); session = {};
    writer.finish(); drain(source, actual); assert(actual == expected && source.all_released());
}
int main() {
    Plain::WriteSession empty;
    assert(!empty && empty.write(std::uint64_t{1}) == Result::error);
    empty.flush();
    empty = {};
    assert(!empty);

    lifecycle(); bounds_and_single_pool(); waiting_overrides_drop(); closed_and_invalid_timeout(); prefixes(); double_loop();
    move_ownership<Plain>(); move_ownership<Framed>(); move_ownership<Sequenced>();
    std::cout << "single-block session, bounds, timeout, prefixes, moves and double-loop tests passed\n";
}
