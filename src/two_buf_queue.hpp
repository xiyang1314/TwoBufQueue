// Copyright 2026 chuan lu
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "TwoBufQueue/src/write/writer.hpp"
#include "TwoBufQueue/src/write/make_queue.hpp"
#include "TwoBufQueue/src/read/read_layer1/read_worker.hpp"
#include "TwoBufQueue/src/read/read_layer1/single_writer/read_worker.hpp"
#include "TwoBufQueue/src/read/layer2/read_block_factory.hpp"
#include "TwoBufQueue/src/read/layer2/to_read_block.hpp"
#include "TwoBufQueue/src/read/layer2/read_callbacks.hpp"
#include "TwoBufQueue/src/base_components/handoff_queue/handoff_queue.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace TwoBufQueue {

// Optional TwoBufQueue / TwoBufQueueBatch constructor options; omitted fields use their defaults.
// Reader/output options only; callers construct and supply the source pool, handoff queue and writer separately.
struct TwoBufQueueOptions {
    // Output side: reader copying and consumer callbacks.
    std::size_t output_block_size = 64 * 1024;   // Bytes per output block.
    std::size_t output_frame_capacity = 64;      // Maximum frames per output block.
    std::size_t output_pool_min = 2;             // Minimum/initial output pool blocks.
    std::size_t output_pool_max = 8;             // Maximum output pool blocks.
    std::size_t output_queue_capacity = 8;       // Output handoff queue capacity.
    // read_ctrl thread count: distribute writers evenly; default is 1.
    std::size_t read_layer1_thread_count = 1;
    // Reader tuning: worker_cpu, idle spins, finish_timeout, etc.
    ReadConfig read_config{};                    // Field defaults are documented in read_layer1/types.hpp.
    CancelClaimMode cancel_claim_mode = CancelClaimMode::QueueTail; // Claim ordering after output-queue cancellation.
    // Buffered output: an independent callback thread by default; one or more copy threads may produce.
    // Non-void Writer / SeqWriter TSyncContext selects synchronous per-record callbacks and bypasses these output options.
    // For TwoBufQueueBatch, Sync still processes each batch synchronously in the copy thread.
    HandoffMode output_mode = HandoffMode::MPSC;
    ReaderMode output_reader_mode = ReaderMode::WaitSignal;
    std::size_t output_reader_count = 1;         // Multiple readers allowed for SPMC/MPMC; ignored by Sync.
    std::chrono::microseconds output_retry_wait{1000}; // Retry delay after callback rejection.
    bool output_enable_stats = false;
};

// Detect sequence-capable WriterT (SeqWriter defines SeqType).
template<class TWriter, class = void>
struct is_seq_writer : std::false_type {};
template<class TWriter>
struct is_seq_writer<TWriter, std::void_t<typename TWriter::SeqType>> : std::true_type {};

template<class TWriter, class = void>
struct is_sync_writer : std::false_type {};
template<class TWriter>
struct is_sync_writer<TWriter, std::void_t<decltype(TWriter::is_sync)>>
    : std::bool_constant<TWriter::is_sync> {};

// Select the per-record callback adapter from WriterT's record layout:
// ordinary Writer uses make_record_callback ([length prefix][payload]);
// SeqWriter uses make_seq_record_callback ([seq][length prefix][payload]) with a leading seq callback argument.
template<class TWriter, class TCallback>
auto make_writer_record_callback(TCallback callback) {
    if constexpr (is_seq_writer<TWriter>::value) {
        return make_seq_record_callback<typename TWriter::SeqType, typename TWriter::DataLen>(
            std::move(callback));
    } else {
        return make_record_callback<typename TWriter::DataLen>(std::move(callback));
    }
}

// Shared internal pipeline skeleton: externally owned WriterT + output HandoffQueue + ReadWorker.
// Writer owns all production/stop interfaces; queue owns reading, copying and consumer callbacks only.
// This layer neither constructs/owns Writer nor forwards writes; applications call their own Writer directly.
template<class TWriter, class TBlockCallback>
class TwoBufQueueBase {
    using OutputPool = ReadBlockPool;
    using OutputQueue = HandoffQueue<OutputPool, TBlockCallback>;
    using Bridge = ToReadBlock<OutputQueue>;
    // Select the pipeline at construction; variant is used only for startup/stop/queries, never in read/callback hot paths.
    struct SingleReader {
        Bridge bridge;
        SingleReadWorker<Bridge> worker;
        SingleReader(OutputQueue& output, TWriter& writer, const TwoBufQueueOptions& options)
            : bridge(output), worker(options.read_config, bridge, writer) { (void)worker.start(); }
        template<class TFunction> void for_each(TFunction function) { function(worker); }
        template<class TFunction> void for_each(TFunction function) const { function(worker); }
    };
    struct MultiReaders {
        // Neither object is movable; deque preserves stable addresses.
        std::deque<Bridge> bridges;
        std::deque<ReadWorker<Bridge>> workers;
        MultiReaders(OutputQueue& output, std::initializer_list<TWriter*> writers,
                     const TwoBufQueueOptions& options) {
            if (writers.size() == 0) return;
            const auto count = std::clamp<std::size_t>(options.read_layer1_thread_count, 1, writers.size());
            for (std::size_t i = 0; i < count; ++i) {
                bridges.emplace_back(output);
                workers.emplace_back(options.read_config, bridges.back());
            }
            std::size_t index = 0;
            for (TWriter* writer : writers)
                if (!workers[index++ % count].add_writer(writer)) return;
            for (auto& worker : workers) (void)worker.start();
        }
        template<class TFunction> void for_each(TFunction function) {
            for (auto& worker : workers) function(worker);
        }
        template<class TFunction> void for_each(TFunction function) const {
            for (const auto& worker : workers) function(worker);
        }
    };

public:
    // Dedicated single-Writer constructor: no Writer list or multi-source Worker.
    TwoBufQueueBase(TWriter& writer, TBlockCallback block_callback, TwoBufQueueOptions options)
        : output_pool_(options.output_pool_min, options.output_pool_max,
                       ReadBlockFactory{options.output_block_size, options.output_frame_capacity}),
          block_callback_(std::move(block_callback)),
          output_queue_(output_pool_, handoff_options(options), std::move(block_callback_)),
          readers_(std::in_place_type<SingleReader>, output_queue_, writer, options) {}

    TwoBufQueueBase(std::initializer_list<TWriter*> writers, TBlockCallback block_callback,
                    TwoBufQueueOptions options)
        : output_pool_(options.output_pool_min, options.output_pool_max,
                       ReadBlockFactory{options.output_block_size, options.output_frame_capacity}),
          block_callback_(std::move(block_callback)),
          output_queue_(output_pool_, handoff_options(options), std::move(block_callback_)),
          readers_(std::in_place_type<MultiReaders>, output_queue_, writers, options) {}

    ~TwoBufQueueBase() noexcept { stop(); }

    TwoBufQueueBase(const TwoBufQueueBase&) = delete;
    TwoBufQueueBase& operator=(const TwoBufQueueBase&) = delete;

    // Stop every reader and wait until every enqueued record is consumed. Idempotent; also called by the destructor.
    // The caller must finish each writer; this layer does not end producers on their behalf.
    void stop() noexcept {
        if (stopped_) return;
        stopped_ = true;
        std::visit([](auto& readers) {
            readers.for_each([](auto& reader) { reader.stop(ReadStopMode::slow); });
        }, readers_);
        output_queue_.close();
        while (!output_queue_.is_drained()) {
            output_queue_.retry_pending();
            std::this_thread::yield();
        }
        output_queue_.stop();
    }

    [[nodiscard]] ReadError error_code() const noexcept {
        ReadError result = ReadError::none;
        std::visit([&](const auto& readers) {
            readers.for_each([&](const auto& reader) {
                if (result == ReadError::none) result = reader.error_code();
            });
        }, readers_);
        return result;
    }
    [[nodiscard]] std::string last_error() const { return error_message(error_code()); }
    [[nodiscard]] bool started() const noexcept {
        bool any = false, all = true;
        std::visit([&](const auto& readers) {
            readers.for_each([&](const auto& reader) { any = true; all &= reader.running(); });
        }, readers_);
        return any && all;
    }
    [[nodiscard]] ReadStats stats() const noexcept {
        ReadStats total{};
        std::visit([&](const auto& readers) {
            readers.for_each([&](const auto& reader) {
                const auto part = reader.stats();
                total.saved_frames += part.saved_frames;
                total.saved_data_bytes += part.saved_data_bytes;
                total.saved_finished_ranges += part.saved_finished_ranges;
                total.saved_partial_frames += part.saved_partial_frames;
                total.scan_growth_count += part.scan_growth_count;
                total.sequence_reset_count += part.sequence_reset_count;
                total.memory_full_events += part.memory_full_events;
                total.memory_backpressured_frames += part.memory_backpressured_frames;
                total.error_count += part.error_count;
            });
        }, readers_);
        return total;
    }

private:
    static HandoffOptions handoff_options(const TwoBufQueueOptions& o) {
        HandoffOptions opts;
        opts.mode = o.output_mode;
        opts.queue_capacity = o.output_queue_capacity;
        opts.reader_mode = o.output_reader_mode;
        opts.reader_count = o.output_reader_count;
        opts.retry_wait = o.output_retry_wait;
        opts.enable_stats = o.output_enable_stats;
        opts.cancel_claim_mode = o.cancel_claim_mode;
        return opts;
    }

#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
public:
    void begin_output_pool_depth_stats() noexcept { output_pool_.begin_depth_stats(); }
    [[nodiscard]] PoolDepthStats end_output_pool_depth_stats() noexcept {
        return output_pool_.end_depth_stats();
    }
private:
#endif

    OutputPool output_pool_;
    TBlockCallback block_callback_;
    OutputQueue output_queue_;
    std::variant<SingleReader, MultiReaders> readers_;
    bool stopped_ = false;
};

// Simple two-buffer record queue with per-record callbacks and caller-constructed writers.
//
// Construct the source pool, handoff queue and WriterT (or SeqWriter), then pass the writer to this class.
// Internally copy source blocks into output blocks; independent callback threads deliver records by default.
// Non-void TSyncContext directly invokes the original callback in the producer's write / write_mem call.
// Writer type is independent of count: one Writer uses no callback lock; multiple Writers share a callback lock.
// Supply multiple writers and read_layer1_thread_count reader threads; distribute writers evenly among them.
//
// Each writer remains single-producer (SPSC); all writers in a queue must use the same WriterT
// and distinct thread_id values, which identify sources in record callbacks.
// WriterT must include a length prefix (TIsWriteLen = true and a DataLen alias).
// Use TwoBufQueueBatch for frameless writers without prefixes, such as Writer<1, false>.
//
// Consumer callback signature for ordinary Writer:
//   void(std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept
// Consumer callback signature for SeqWriter:
//   void(std::int32_t thread_id, TSeq seq, const std::byte* data, std::size_t size) noexcept
// thread_id identifies the source. data is valid only until callback return; never retain its pointer. Ignore callback return values.
//
// Single-writer usage:
//   auto writer = make_writer<Writer<>>();
//   TwoBufQueue queue([](std::int32_t tid, const std::byte* d, std::size_t n) noexcept {}, writer);
// Multiple writers/readers with evenly assigned writers:
//   TwoBufQueue queue(callback, {&w0, &w1, &w2, &w3}, {.read_layer1_thread_count = 2});
//   writer.write_mem("hello", 5);
//   writer.finish();   // The application finishes the producer.
//   queue.stop();      // Consume every record and stop the threads.
template<class TRecordCallback, class TWriter>
class TwoBufQueue
    : public TwoBufQueueBase<TWriter,
          decltype(make_writer_record_callback<TWriter>(std::declval<TRecordCallback>()))> {
    using Base = TwoBufQueueBase<TWriter,
        decltype(make_writer_record_callback<TWriter>(std::declval<TRecordCallback>()))>;
    static_assert(!is_sync_writer<TWriter>::value, "sync selection must match the Writer type");

public:
    // Single writer: the most common case.
    TwoBufQueue(TRecordCallback callback, TWriter& writer, TwoBufQueueOptions options = {})
        : Base(writer, make_writer_record_callback<TWriter>(std::move(callback)), record_options(options)) {}

    // Multiple writers share WriterT and have unique thread_id values; read_layer1_thread_count optionally enables multiple readers.
    TwoBufQueue(TRecordCallback callback, std::initializer_list<TWriter*> writers,
                 TwoBufQueueOptions options = {})
        : Base(writers, make_writer_record_callback<TWriter>(std::move(callback)), record_options(options)) {}

private:
    static TwoBufQueueOptions record_options(TwoBufQueueOptions options) {
        if (options.output_mode == HandoffMode::Sync)
            throw std::invalid_argument("producer-thread sync callbacks require a non-void Writer TSyncContext");
        return options;
    }
};

// The synchronous path retains the original callback Context and borrowed Writers only, without output pools or reader threads.
// Finish Writers and join producer threads before stop; only empty source blocks from construction/finish are returned here.
template<class TRecordCallback, class TWriter>
    requires (is_sync_writer<TWriter>::value)
class TwoBufQueue<TRecordCallback, TWriter> {
    static_assert(is_sync_writer<TWriter>::value, "sync selection must match the Writer type");
    using Context = typename TWriter::SyncContextType;
    static_assert(std::is_same_v<TRecordCallback, typename Context::CallbackType>,
                  "queue callback must match the SyncContext callback type");
    static_assert([] {
        if constexpr (is_seq_writer<TWriter>::value)
            return Context::template accepts_record<std::int32_t,
                typename TWriter::SeqType, const std::byte*, std::size_t>;
        else
            return Context::template accepts_record<std::int32_t,
                const std::byte*, std::size_t>;
    }(), "sync callback must use the ordinary / SeqWriter noexcept record signature");

public:
    TwoBufQueue(TRecordCallback callback, TWriter& writer, TwoBufQueueOptions = {})
        : context_(std::move(callback), false), writers_(&writer) {
        validate_writer(writer);
        writer.set_sync_callback(context_);
    }

    TwoBufQueue(TRecordCallback callback, std::initializer_list<TWriter*> writers,
                TwoBufQueueOptions = {})
        : context_(std::move(callback), writers.size() > 1),
          writers_(std::in_place_type<std::vector<TWriter*>>, writers) {
        if (writers.size() == 0) throw std::invalid_argument("sync queue requires at least one writer");
        const auto& list = std::get<std::vector<TWriter*>>(writers_);
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (!list[i]) throw std::invalid_argument("sync writer must not be null");
            validate_writer(*list[i]);
            for (std::size_t j = 0; j < i; ++j)
                if (list[i] == list[j] || list[i]->thread_id() == list[j]->thread_id() ||
                    &list[i]->handoff_queue() == &list[j]->handoff_queue())
                    throw std::invalid_argument("sync writers and thread ids must be unique");
        }
        for (auto* writer : list) writer->set_sync_callback(context_);
    }

    ~TwoBufQueue() noexcept { stop(); }
    TwoBufQueue(const TwoBufQueue&) = delete;
    TwoBufQueue& operator=(const TwoBufQueue&) = delete;

    void stop() noexcept {
        if (stopped_) return;
        stopped_ = true;
        for_each_writer([&](TWriter& writer) {
            writer.clear_sync_callback(context_);
            auto& source = writer.handoff_queue();
            if (source.current_producer()) {
                std::fputs("sync queue stop requires writer.finish() after stopping production\n", stderr);
                std::terminate();
            }
            source.close();
            while (auto record = source.try_pop()) {
                // Synchronous write does not advance source cursors, so the original lifecycle contains only begin records and empty completed blocks.
                if (record.get() && !record->readable().empty()) {
                    std::fputs("sync writer unexpectedly contains buffered data\n", stderr);
                    std::terminate();
                }
                source.finish(record);
            }
            if (!source.is_drained()) std::terminate();
        });
    }

    // In synchronous mode started means the callback binding is usable; copy-side statistics remain zero.
    [[nodiscard]] bool started() const noexcept { return !stopped_; }
    [[nodiscard]] ReadError error_code() const noexcept { return ReadError::none; }
    [[nodiscard]] std::string last_error() const { return error_message(error_code()); }
    [[nodiscard]] ReadStats stats() const noexcept { return {}; }

private:
    static void validate_writer(const TWriter& writer) {
        if (writer.thread_id() < 0 || !writer.handoff_queue().current_producer() ||
            writer.handoff_queue().is_closed() || writer.handoff_queue().has_read_available_callback() ||
            writer.has_sync_callback())
            throw std::invalid_argument("sync writer must be live and exclusively bound to this queue");
    }
    template<class TFunction>
    void for_each_writer(TFunction function) noexcept {
        if (auto* single = std::get_if<TWriter*>(&writers_)) function(**single);
        else for (auto* writer : std::get<std::vector<TWriter*>>(writers_)) function(*writer);
    }

    Context context_;
    std::variant<TWriter*, std::vector<TWriter*>> writers_;
    bool stopped_ = false;
};

// Batch-callback form of the two-buffer queue: the callback receives the entire DataBatch (read-only frame views).
// true accepts the whole batch; false leaves it unhandled for a complete retry next round.
// Suitable for frameless writers such as Writer<1, false>, or applications processing whole batches for throughput.
template<class TBatchCallback, class TWriter>
class TwoBufQueueBatch
    : public TwoBufQueueBase<TWriter,
          decltype(make_batch_callback(std::declval<TBatchCallback>()))> {
    using Base = TwoBufQueueBase<TWriter,
        decltype(make_batch_callback(std::declval<TBatchCallback>()))>;
    static_assert(!is_sync_writer<TWriter>::value,
                  "non-void TSyncContext supports per-record TwoBufQueue callbacks, not DataBatch callbacks");

public:
    TwoBufQueueBatch(TBatchCallback callback, TWriter& writer, TwoBufQueueOptions options = {})
        : Base(writer, make_batch_callback(std::move(callback)), options) {}

    TwoBufQueueBatch(TBatchCallback callback, std::initializer_list<TWriter*> writers,
                      TwoBufQueueOptions options = {})
        : Base(writers, make_batch_callback(std::move(callback)), options) {}
};

// Class template argument deduction obtains WriterT from the supplied writer, including ordinary Writer and SeqWriter.
template<class TRecordCallback, class TWriter>
TwoBufQueue(TRecordCallback, TWriter&, TwoBufQueueOptions = {}) -> TwoBufQueue<TRecordCallback, TWriter>;

template<class TRecordCallback, class TWriter>
TwoBufQueue(TRecordCallback, std::initializer_list<TWriter*>, TwoBufQueueOptions = {}) -> TwoBufQueue<TRecordCallback, TWriter>;

template<class TBatchCallback, class TWriter>
TwoBufQueueBatch(TBatchCallback, TWriter&, TwoBufQueueOptions = {}) -> TwoBufQueueBatch<TBatchCallback, TWriter>;

template<class TBatchCallback, class TWriter>
TwoBufQueueBatch(TBatchCallback, std::initializer_list<TWriter*>, TwoBufQueueOptions = {}) -> TwoBufQueueBatch<TBatchCallback, TWriter>;

} // namespace TwoBufQueue
