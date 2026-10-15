#include "../support/writer_fixture.hpp"
#pragma once
#include "TwoBufQueue/src/base_components/block_slice/slice_pool.hpp"
#include "TwoBufQueue/src/base_components/handoff_queue/pooled_resource_queue.hpp"
#include "TwoBufQueue/src/read/read_layer1/read_service.hpp"
#include "TwoBufQueue/src/read/read_layer1/read_worker.hpp"
#include "TwoBufQueue/src/read/layer2/read_block_factory.hpp"
#include "TwoBufQueue/src/read/layer2/read_callbacks.hpp"
#include "TwoBufQueue/src/read/layer2/to_read_block.hpp"
#include "TwoBufQueue/src/write/writer.hpp"
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <random>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ft {
using namespace TwoBufQueue;
using namespace std::chrono_literals;
inline void check(bool ok, const char *expr,
                  std::source_location at = std::source_location::current()) {
    if (!ok)
        throw std::runtime_error(std::string(at.file_name()) + ":" + std::to_string(at.line()) +
                                 " " + expr);
}
#define CHECK(...) ::ft::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__)
template <class TFunction> void invalid(TFunction &&f) {
    bool caught = false;
    try {
        f();
    } catch (const std::invalid_argument &) {
        caught = true;
    }
    CHECK(caught);
}
struct Case {
    std::string id;
    std::function<void()> run;
};
using Cases = std::vector<Case>;
void write_read_cases(Cases &);
void write_layout_cases(Cases &);
void store_cases(Cases &);
void pool_cases(Cases &);
void concurrency_cases(Cases &);
void death_cases(Cases &);
void policy_cases(Cases &);
inline std::vector<std::byte> payload(std::size_t n, unsigned seed = 17) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = std::byte((i * 131 + seed * 17) & 255);
    return v;
}
struct Message {
    int id;
    std::vector<std::byte> bytes;
    bool operator==(const Message &) const = default;
};
// Implement all six ReadWriterDataCallback methods; read_complete / read_current parse length-prefixed records block by block.
// end_read follows every read_once, whether that round succeeds or fails.
struct Receiver {
    std::atomic<bool> reject{false};
    bool good = true;
    std::size_t calls = 0, raw_bytes = 0;
    std::vector<Message> messages;

    bool parse(const std::byte *data, std::size_t size, int source_id) noexcept {
        std::size_t offset = 0;
        while (offset < size) {
            if (size - offset < 2) {
                good = false;
                return false;
            }
            const auto *d = data + offset;
            const auto n = std::to_integer<unsigned>(d[0]) + (std::to_integer<unsigned>(d[1]) << 8);
            offset += 2;
            if (n > size - offset) {
                good = false;
                return false;
            }
            try {
                messages.push_back({source_id, {data + offset, data + offset + n}});
            } catch (...) {
                good = false;
                return false;
            }
            offset += n;
        }
        return true;
    }
    void begin_read_complete(std::int64_t) noexcept {}
    bool read_complete(SpscWriteBlock *block) noexcept {
        ++calls;
        if (reject.load(std::memory_order_relaxed))
            return false;
        const auto bytes = block->readable();
        if (!parse(bytes.data(), bytes.size(), block->source_id()))
            return false;
        raw_bytes += bytes.size();
        return true;
    }
    void end_read_complete() noexcept {}
    void begin_read_current(std::int64_t) noexcept {}
    bool read_current(SpscWriteBlock *block, const std::byte *ptr, std::size_t size) noexcept {
        ++calls;
        if (reject.load(std::memory_order_relaxed))
            return false;
        if (!parse(ptr, size, block->source_id()))
            return false;
        raw_bytes += size;
        return true;
    }
    void end_read() noexcept {}
};
template <class TCallback>
inline void tick(ReadService<TCallback> &service) {
    const std::atomic<bool> no{false};
    (void)service.read_once(true, no);
}
template <unsigned TPublish = 1, std::size_t TMin = 0> struct Direct {
    WriterFixture pool;
    Writer<TPublish, true> writer;
    Receiver receiver;
    AutoResetEvent event;
    ReadService<Receiver> service;
    Direct(std::size_t bytes = 256, std::size_t blocks = 4, WriterBackpressureConfig bp = {})
        : pool(bytes, blocks, blocks, bp, TMin), writer(&pool.queue, 3, pool.backpressure),
          service({}, receiver, event) {
        CHECK(service.add_writer(&writer, false));
        tick(service);
    }
    void drain() {
        writer.finish();
        for (int i = 0; i < 32; ++i)
            tick(service);
        CHECK(receiver.good);
        CHECK(service.error_code() == ReadError::none);
    }
    ~Direct() {
        receiver.reject = false;
        writer.finish();
        for (int i = 0; i < 32; ++i)
            tick(service);
    }
};
inline std::vector<std::byte> framed(unsigned value) {
    std::vector<std::byte> v(6);
    v[0] = std::byte{4};
    v[1] = std::byte{0};
    std::memcpy(v.data() + 2, &value, 4);
    return v;
}
// Buffered read pipeline: Writer -> ReadService -> ToReadBlock -> PooledResourceQueue.
struct CollectedFrame {
    std::int32_t thread_id = -1;
    std::vector<std::byte> bytes;
    std::int64_t read_time_ns = 0;
};
struct CollectedBatch {
    std::size_t data_size = 0;
    std::vector<CollectedFrame> frames;
    bool empty() const { return frames.empty(); }
};
inline CollectedBatch collect(const DataBatch &batch) {
    CollectedBatch out;
    out.data_size = batch.data_size;
    for (const auto &frame : batch.frames)
        out.frames.push_back({frame.thread_id, {frame.data, frame.data + frame.size}, frame.read_time_ns});
    return out;
}
using OutputQueue = PooledResourceQueue<ReadBlockPool>;
using StoreSaver = ToReadBlock<OutputQueue>;
template <unsigned TPublish = 1, std::size_t TMin = 0, class TDataLen = std::uint16_t> struct Buffered {
    WriterFixture source;
    Writer<TPublish, true, TDataLen> writer;
    ReadBlockPool pool;
    OutputQueue output;
    StoreSaver saver;
    AutoResetEvent event;
    ReadService<StoreSaver> service;
    Buffered(std::size_t src_bytes, std::size_t src_min, std::size_t src_max,
             std::size_t block_capacity, std::size_t frame_capacity,
             std::size_t pool_min, std::size_t pool_max, std::size_t queue_capacity,
             WriterBackpressureConfig bp = {})
        : source(src_bytes, src_min, src_max, bp),
          writer(&source.queue, 3, source.backpressure),
          pool(pool_min, pool_max, ReadBlockFactory{block_capacity, frame_capacity}),
          output(pool, queue_capacity), saver(output), service({}, saver, event) {
        CHECK(service.add_writer(&writer, false));
        tick(service);
    }
    void finish() {
        writer.finish();
        for (int i = 0; i < 32; ++i)
            tick(service);
    }
    bool try_read(CollectedBatch &out) {
        auto loan = output.try_receive();
        if (!loan)
            return false;
        out = collect(to_data_batch(*loan));
        CHECK(output.finish(loan));
        return true;
    }
    bool all_released() const { return pool.all_released(); }
    std::size_t block_count() const { return pool.resource_count(); }
};
} // namespace ft
