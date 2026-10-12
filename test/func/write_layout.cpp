#include "../writer_fixture.hpp"
#include "support.hpp"
#include <limits>
#include <type_traits>

namespace ft {
namespace {
// 不借用实现自身解码，只按来源编号拼接完整字节流与独立编码逐字节比对。
struct RawReceiver {
    std::array<std::vector<std::byte>, 4> bytes;
    bool reject = false, good = true;
    bool accumulate(std::int32_t id, const std::byte *data, std::size_t size) noexcept {
        if (id < 0 || id >= 4 || size == 0) { good = false; return false; }
        try {
            auto &output = bytes[static_cast<std::size_t>(id)];
            output.insert(output.end(), data, data + size);
            return true;
        } catch (...) { good = false; return false; }
    }
    void begin_read_complete(std::int64_t) noexcept {}
    bool read_complete(SpscWriteBlock *block) noexcept {
        if (reject) return false;
        const auto r = block->readable();
        return accumulate(block->source_id(), r.data(), r.size());
    }
    void end_read_complete() noexcept {}
    void begin_read_current(std::int64_t) noexcept {}
    bool read_current(SpscWriteBlock *block, const std::byte *ptr, std::size_t size) noexcept {
        if (reject) return false;
        return accumulate(block->source_id(), ptr, size);
    }
    void end_read() noexcept {}
};

template<class T> void append_value(std::vector<std::byte>& bytes, const T& value) {
    const auto* begin = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), begin, begin + sizeof(T));
}
template<bool WriteLen, class Len>
void append_record(std::vector<std::byte>& output, const std::vector<std::byte>& payload) {
    if constexpr (WriteLen) {
        auto size = static_cast<std::uint64_t>(payload.size());
        for (std::size_t i = 0; i < sizeof(Len); ++i) {
            output.push_back(std::byte(size % 256));
            size /= 256;
        }
    }
    output.insert(output.end(), payload.begin(), payload.end());
}

template<unsigned Publish, bool WriteLen, class Len>
void layout_bytes(bool buffered) {
    using RecordWriter = std::conditional_t<WriteLen,
        Writer<Publish, true, Len>, Writer<Publish, false>>;
    if constexpr (WriteLen) {
        using RawWrite = bool (RecordWriter::*)(const void*, Len) noexcept;
        [[maybe_unused]] RawWrite typed_entry = static_cast<RawWrite>(&RecordWriter::write_mem);
    }
    static_assert(!std::is_polymorphic_v<RecordWriter>);
    WriterFixture pool(32, 3, 3, {}, 1, RecordWriter::message_prefix_size);
    RecordWriter writer(&pool.queue, 2, pool.backpressure);
    RawReceiver receiver;
    AutoResetEvent event;
    std::vector<std::byte> expected;

    auto write_all = [&](auto &&pump) {
        for (unsigned i = 0; i < 25; ++i) {
            std::uint8_t a = static_cast<std::uint8_t>(i);
            std::uint32_t b = 0x12345678u ^ i;
            std::uint16_t c = static_cast<std::uint16_t>(65500 + i);
            std::vector<std::byte> packed;
            append_value(packed, a); append_value(packed, b); append_value(packed, c);
            auto* begin = writer.published_write_ptr();
            CHECK(writer.write(a, b, c));
            if (i == 0) {
                CHECK(writer.published_write_ptr() == begin +
                      (Publish == 1 ? RecordWriter::message_prefix_size + packed.size() : 0));
            }
            append_record<WriteLen, Len>(expected, packed);
            auto raw = payload(3, i);
            CHECK(writer.write_mem(raw.data(), static_cast<Len>(raw.size())));
            append_record<WriteLen, Len>(expected, raw);
            std::fill(raw.begin(), raw.end(), std::byte{0});
            writer.flush();
            if (i == 0 && !buffered) {
                receiver.reject = true;
                pump();
                CHECK(receiver.bytes[2].empty());
                receiver.reject = false;
            }
            pump();
        }
        writer.finish(); pump();
    };

    if (buffered) {
        ReadBlockPool out_pool(2, 8, ReadBlockFactory{64, 64});
        OutputQueue output(out_pool, 8);
        ToReadBlock saver(output);
        ReadService service({}, saver, event);
        CHECK(service.add_writer(&writer, false));
        auto pump = [&] {
            for (int i = 0; i < 4; ++i) tick(service);
            for (;;) {
                auto loan = output.try_receive();
                if (!loan) break;
                auto db = to_data_batch(*loan);
                for (const auto &frame : db.frames)
                    receiver.accumulate(frame.thread_id, frame.data, frame.size);
                CHECK(output.finish(loan));
            }
        };
        write_all(pump);
        CHECK(receiver.good && receiver.bytes[2] == expected);
        CHECK(service.error_code() == ReadError::none);
        CHECK(service.stats().saved_data_bytes == expected.size());
        output.close();
        while (!output.is_drained()) {
            auto loan = output.try_receive();
            if (loan) CHECK(output.finish(loan));
        }
        CHECK(out_pool.all_released());
        CHECK(pool.all_released());
    } else {
        ReadService service({}, receiver, event);
        CHECK(service.add_writer(&writer, false));
        auto pump = [&] { for (int i = 0; i < 4; ++i) tick(service); };
        write_all(pump);
        CHECK(receiver.good && receiver.bytes[2] == expected);
        CHECK(service.error_code() == ReadError::none);
        CHECK(service.stats().saved_data_bytes == expected.size());
        CHECK(pool.all_released());
    }
}

template<bool WriteLen, class Len> void slice_capacity() {
    using RecordWriter = std::conditional_t<WriteLen,
        Writer<1, true, Len>, Writer<1, false>>;
    constexpr std::size_t count = 1100;
    constexpr std::size_t block_bytes = count * (RecordWriter::message_prefix_size + 1);
    WriterFixture pool(block_bytes, 1, 1, {}, 0, RecordWriter::message_prefix_size);
    RecordWriter writer(&pool.queue, 0, pool.backpressure);
    RawReceiver receiver;
    AutoResetEvent event;
    ReadService service({}, receiver, event);
    CHECK(service.add_writer(&writer, false)); tick(service);
    std::vector<std::byte> expected;
    for (unsigned i = 0; i < count; ++i) {
        const auto value = static_cast<std::uint8_t>(i);
        CHECK(writer.write(value));
        append_record<WriteLen, Len>(expected, {std::byte{value}});
        tick(service); tick(service);
    }
    CHECK(receiver.good && receiver.bytes[0] == expected);
    CHECK(writer.source_block()->read_pos() == block_bytes);
    writer.finish(); tick(service);
    CHECK(pool.all_released());
}

template<class Len> void length_limits() {
    using RecordWriter = Writer<1, true, Len>;
    const auto maximum = (std::numeric_limits<Len>::max)();
    // 选一个既容纳于 Len、又为“超出块容量”留出余量的有效长度。
    const std::size_t valid_size = sizeof(Len) == 1 ? 200u : sizeof(Len) == 2 ? 60000u : 100000u;
    WriterFixture pool(valid_size + sizeof(Len), 1, 1, {}, 0, sizeof(Len));
    RecordWriter writer(&pool.queue, 0, pool.backpressure);
    RawReceiver receiver;
    AutoResetEvent event;
    ReadService service({}, receiver, event);
    CHECK(service.add_writer(&writer, false)); tick(service);
    const auto bytes = payload(valid_size);
    auto* before = writer.published_write_ptr();
    CHECK(!writer.write_mem(nullptr, Len{1}));        // 空指针 + 正长度
    CHECK(!writer.write_mem(bytes.data(), maximum));  // 超出块容量（u64 还触发前缀加法溢出）
    CHECK(writer.published_write_ptr() == before);
    CHECK(writer.write_mem(bytes.data(), static_cast<Len>(valid_size)));
    writer.finish(); tick(service);
    std::vector<std::byte> expected;
    append_record<true, Len>(expected, bytes);
    CHECK(receiver.good && receiver.bytes[0] == expected && pool.all_released());
}

void empty_and_config() {
    static_assert(source_slice_capacity(120, 4, 0) == 31);
    static_assert(source_slice_capacity(120, 4, 1) == 25);
    static_assert(source_slice_capacity(120, 4, 2) == 21);
    static_assert(source_slice_capacity(120, 4, 4) == 16);
    static_assert(source_slice_capacity(120, 4, 8) == 11);
    static_assert(source_slice_capacity(8, 0, 0) == 9);
    invalid([] { (void)source_slice_capacity(0, 0, 0); });
    invalid([] { (void)source_slice_capacity(8, 0, 3); });
    invalid([] { (void)source_slice_capacity(512, 256, 1); });
    invalid([] { (void)source_slice_capacity(8, 1, 8); });
    invalid([] { (void)source_slice_capacity((std::numeric_limits<std::size_t>::max)(), 0, 0); });
    invalid([] { (void)source_slice_capacity((std::numeric_limits<std::size_t>::max)(),
                                           (std::numeric_limits<std::size_t>::max)(), 8); });
    WriterFixture default_pool(16, 1, 1);
    invalid([] { Writer<> wrong(nullptr, 0); });
    CHECK(default_pool.all_released() && default_pool.handoff_queue().empty());
    WriterFixture pool(1, 1, 1, {}, 0, 0);
    Writer<1, false> writer(&pool.queue, 0, pool.backpressure);
    const std::uint8_t value = 42;
    const auto* before = writer.published_write_ptr();
    CHECK(!writer.write_mem(nullptr, std::uint16_t{0}));
    CHECK(!writer.write_mem(&value, std::uint16_t{0}));
    CHECK(!writer.write_mem(&value, (std::numeric_limits<std::uint16_t>::max)()));
    CHECK(writer.published_write_ptr() == before);
    CHECK(writer.write(value)); // A one-byte block is valid without a prefix.
    RawReceiver receiver;
    AutoResetEvent event;
    ReadService service({}, receiver, event);
    CHECK(service.add_writer(&writer, false));
    writer.finish(); tick(service);
    CHECK(receiver.bytes[0] == std::vector<std::byte>{std::byte{42}});
}

void mixed_registration(bool background) {
    WriterFixture a(128, 2, 4, {}, 0, 1), b(128, 2, 4, {}, 0, 0), other(128, 1, 1);
    Writer<1, true, std::uint8_t> first(&a.queue, 1, a.backpressure);
    Writer<3, false> second(&b.queue, 2, b.backpressure);
    RawReceiver receiver;
    std::vector<std::byte> expected_a, expected_b;
    auto produce = [&] {
        for (std::uint32_t i = 0; i < 40; ++i) {
            CHECK(first.write(i)); CHECK(second.write(i));
            std::vector<std::byte> bytes; append_value(bytes, i);
            append_record<true, std::uint8_t>(expected_a, bytes);
            append_record<false, std::uint64_t>(expected_b, bytes);
        }
        first.finish(); second.finish();
    };
    if (background) {
        ReadWorker worker({}, receiver);
        CHECK(worker.add_writer(&first)); CHECK(worker.add_writer(&second));
        CHECK(!worker.add_writer(static_cast<decltype(first)*>(nullptr)));
        CHECK(worker.start()); produce(); worker.stop();
        CHECK(worker.stats().saved_data_bytes == expected_a.size() + expected_b.size());
    } else {
        AutoResetEvent event;
        ReadService service({}, receiver, event);
        CHECK(!service.add_queue(nullptr, 1));
        CHECK(service.error_code() == ReadError::null_dependency);
        CHECK(service.add_writer(&first, false));
        CHECK(service.add_writer(&second, false));
        CHECK(!service.add_writer(&first, false));
        CHECK(service.error_code() == ReadError::duplicate_writer);
        CHECK(!service.add_writer(static_cast<decltype(second)*>(nullptr), false));
        CHECK(service.error_code() == ReadError::null_dependency);
        produce(); for (int i = 0; i < 6; ++i) tick(service);
        CHECK(service.writer_count() == 2);
    }
    CHECK(receiver.good && receiver.bytes[1] == expected_a && receiver.bytes[2] == expected_b);
    CHECK(a.all_released() && b.all_released());
}
} // namespace

void write_layout_cases(Cases& cases) {
    auto add = [&]<class Len>(const std::string& name) {
        auto mode = [&]<bool WriteLen>(const std::string& suffix) {
            cases.push_back({"W_layout_" + name + suffix + "_p1", [] { layout_bytes<1, WriteLen, Len>(false); }});
            cases.push_back({"W_layout_" + name + suffix + "_p3", [] { layout_bytes<3, WriteLen, Len>(false); }});
            cases.push_back({"W_layout_" + name + suffix + "_buffered", [] { layout_bytes<3, WriteLen, Len>(true); }});
            cases.push_back({"W_slots_" + name + suffix, [] { slice_capacity<WriteLen, Len>(); }});
        };
        mode.template operator()<true>("_prefix");
        mode.template operator()<false>("_raw");
        cases.push_back({"W_limits_" + name, [] { length_limits<Len>(); }});
    };
    add.template operator()<std::uint8_t>("u8");
    add.template operator()<std::uint16_t>("u16");
    add.template operator()<std::uint32_t>("u32");
    add.template operator()<std::uint64_t>("u64");
    cases.push_back({"W_layout_empty_config", empty_and_config});
    cases.push_back({"W_layout_registration", [] { mixed_registration(false); }});
    cases.push_back({"W_layout_worker", [] { mixed_registration(true); }});
}
} // namespace ft
