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
#include "TwoBufQueue/src/base_components/HandoffQueue/handoff_queue.hpp"

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

// TwoBufQueue / TwoBufQueueBatch 的构造参数，全部可选，不传即用默认值。
// 只包含读取侧（输出）参数；源侧（源块池 / 交接队列 / writer）由用户自行构造并传入。
struct TwoBufQueueOptions {
    // ── 输出读取侧（读取线程复制 + 消费回调） ──
    std::size_t output_block_size = 64 * 1024;   // 单个输出块字节数
    std::size_t output_frame_capacity = 64;      // 每个输出块最多 frame 数
    std::size_t output_pool_min = 2;             // 输出块池最小/初始数量
    std::size_t output_pool_max = 8;             // 输出块池最大数量
    std::size_t output_queue_capacity = 8;       // 输出块交接队列容量
    // ── 读取线程（read_ctrl）数量：writer 均分到各线程，默认 1 ──
    std::size_t read_layer1_thread_count = 1;
    // ── 读取线程调优（worker_cpu / idle spin / finish_timeout 等） ──
    ReadConfig read_config{};                    // 各字段默认值见 read_layer1/types.hpp
    CancelClaimMode cancel_claim_mode = CancelClaimMode::QueueTail; // 输出队列取消后的领取顺序
    // 缓冲输出消费配置：默认由独立回调线程消费，支持一个或多个复制线程生产。
    // 逐条同步回调由 Writer / SeqWriter 的非 void TSyncContext 选择，不使用这些输出参数。
    // TwoBufQueueBatch 的 Sync 仍表示在复制线程同步处理整批。
    HandoffMode output_mode = HandoffMode::MPSC;
    ReaderMode output_reader_mode = ReaderMode::WaitSignal;
    std::size_t output_reader_count = 1;         // SPMC/MPMC 可设置多个；Sync 忽略。
    std::chrono::microseconds output_retry_wait{1000}; // 回调拒绝后的重试等待。
    bool output_enable_stats = false;
};

// 判断 WriterT 是否带 seq（SeqWriter 定义了 SeqType 别名）。
template<class WriterT, class = void>
struct is_seq_writer : std::false_type {};
template<class WriterT>
struct is_seq_writer<WriterT, std::void_t<typename WriterT::SeqType>> : std::true_type {};

template<class WriterT, class = void>
struct is_sync_writer : std::false_type {};
template<class WriterT>
struct is_sync_writer<WriterT, std::void_t<decltype(WriterT::is_sync)>>
    : std::bool_constant<WriterT::is_sync> {};

// 按 WriterT 的记录布局选择合适的逐条解析回调：
// 普通 Writer 用 make_record_callback（[长度前缀][负载]）；
// SeqWriter 用 make_seq_record_callback（[seq][长度前缀][负载]，回调多一个 seq 首参）。
template<class WriterT, class Callback>
auto make_writer_record_callback(Callback callback) {
    if constexpr (is_seq_writer<WriterT>::value) {
        return make_seq_record_callback<typename WriterT::SeqType, typename WriterT::DataLen>(
            std::move(callback));
    } else {
        return make_record_callback<typename WriterT::DataLen>(std::move(callback));
    }
}

// 双缓冲管道的公共骨架（内部实现）：组装外部 WriterT + 输出侧 HandoffQueue + ReadWorker。
// 分两层：一层是 writer（用户构造、拥有全部写/停接口），一层是 queue（只管读取复制与消费回调）。
// 因此本层不构造、不拥有 writer，也不代理任何写接口；用户直接调用自己的 writer。
template<class WriterT, class BlockCallback>
class TwoBufQueueBase {
    using OutputPool = ReadBlockPool;
    using OutputQueue = HandoffQueue<OutputPool, BlockCallback>;
    using Bridge = ToReadBlock<OutputQueue>;
    // 构造时选择管道；variant 仅用于启动/停止/查询，不进入读取或回调热路径。
    struct SingleReader {
        Bridge bridge;
        SingleReadWorker<Bridge> worker;
        SingleReader(OutputQueue& output, WriterT& writer, const TwoBufQueueOptions& options)
            : bridge(output), worker(options.read_config, bridge, writer) { (void)worker.start(); }
        template<class TFunction> void for_each(TFunction function) { function(worker); }
        template<class TFunction> void for_each(TFunction function) const { function(worker); }
    };
    struct MultiReaders {
        // 两种对象不可移动，用地址稳定的 deque 保存。
        std::deque<Bridge> bridges;
        std::deque<ReadWorker<Bridge>> workers;
        MultiReaders(OutputQueue& output, std::initializer_list<WriterT*> writers,
                     const TwoBufQueueOptions& options) {
            if (writers.size() == 0) return;
            const auto count = std::clamp<std::size_t>(options.read_layer1_thread_count, 1, writers.size());
            for (std::size_t i = 0; i < count; ++i) {
                bridges.emplace_back(output);
                workers.emplace_back(options.read_config, bridges.back());
            }
            std::size_t index = 0;
            for (WriterT* writer : writers)
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
    // 独立的单 Writer 构造路径，不建立 Writer 列表或多来源 Worker。
    TwoBufQueueBase(WriterT& writer, BlockCallback block_callback, TwoBufQueueOptions options)
        : output_pool_(options.output_pool_min, options.output_pool_max,
                       ReadBlockFactory{options.output_block_size, options.output_frame_capacity}),
          block_callback_(std::move(block_callback)),
          output_queue_(output_pool_, handoff_options(options), std::move(block_callback_)),
          readers_(std::in_place_type<SingleReader>, output_queue_, writer, options) {}

    TwoBufQueueBase(std::initializer_list<WriterT*> writers, BlockCallback block_callback,
                    TwoBufQueueOptions options)
        : output_pool_(options.output_pool_min, options.output_pool_max,
                       ReadBlockFactory{options.output_block_size, options.output_frame_capacity}),
          block_callback_(std::move(block_callback)),
          output_queue_(output_pool_, handoff_options(options), std::move(block_callback_)),
          readers_(std::in_place_type<MultiReaders>, output_queue_, writers, options) {}

    ~TwoBufQueueBase() noexcept { stop(); }

    TwoBufQueueBase(const TwoBufQueueBase&) = delete;
    TwoBufQueueBase& operator=(const TwoBufQueueBase&) = delete;

    // 停止全部读取线程，并等待所有已入队记录被消费完。幂等；析构时也会调用。
    // 注意：writer 的 finish() 由用户负责，本层不代为结束生产者。
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
    BlockCallback block_callback_;
    OutputQueue output_queue_;
    std::variant<SingleReader, MultiReaders> readers_;
    bool stopped_ = false;
};

// 简单易用的「双缓冲记录队列」，逐条回调；writer 由用户构造后传入。
//
//   用户先构造源块池 + 交接队列 + WriterT（或 SeqWriter），再把它传给本类；
//   本类内部把源块复制到输出块，默认由独立回调线程按条交付。
//   非 void TSyncContext 在 write / write_mem 所在的写线程直接调用原始回调。
//   Writer 类型不区分数量：构造时只有一个 Writer 则无锁，多个 Writer 则共享回调锁。
//   可传入多个 writer 和多个读取线程（read_layer1_thread_count），writer 均分到各线程。
//
// 约束：每个 writer 仍是单生产者（SPSC）；同一队列的多个 writer 必须是同一 WriterT，
// 且各自 thread_id 互不相同（记录回调据此区分来源）。
// WriterT 必须是带长度前缀的写入器（TIsWriteLen=true，定义了 DataLen）；
// 无前缀的 frameless writer（如 Writer<1,false>）请用 TwoBufQueueBatch。
//
// 消费回调签名（普通 Writer）：
//   void(std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept
// 消费回调签名（SeqWriter）：
//   void(std::int32_t thread_id, TSeq seq, const std::byte* data, std::size_t size) noexcept
// thread_id 是记录来源编号；data 只在回调返回前有效，不得保存指针；回调返回值被忽略。
//
// 用法（单 writer）：
//   auto writer = make_writer<Writer<>>();
//   TwoBufQueue queue([](std::int32_t tid, const std::byte* d, std::size_t n) noexcept {}, writer);
// 用法（多 writer + 多读线程，writer 均分）：
//   TwoBufQueue queue(callback, {&w0, &w1, &w2, &w3}, {.read_layer1_thread_count = 2});
//   writer.write_mem("hello", 5);
//   writer.finish();   // 生产者结束（由用户负责）
//   queue.stop();      // 等待全部记录消费完并停止线程
template<class RecordCallback, class WriterT>
class TwoBufQueue
    : public TwoBufQueueBase<WriterT,
          decltype(make_writer_record_callback<WriterT>(std::declval<RecordCallback>()))> {
    using Base = TwoBufQueueBase<WriterT,
        decltype(make_writer_record_callback<WriterT>(std::declval<RecordCallback>()))>;
    static_assert(!is_sync_writer<WriterT>::value, "sync selection must match the Writer type");

public:
    // 单 writer（最常用）。
    TwoBufQueue(RecordCallback callback, WriterT& writer, TwoBufQueueOptions options = {})
        : Base(writer, make_writer_record_callback<WriterT>(std::move(callback)), record_options(options)) {}

    // 多 writer：同一 WriterT、thread_id 唯一；可选 read_layer1_thread_count 分多个读线程。
    TwoBufQueue(RecordCallback callback, std::initializer_list<WriterT*> writers,
                 TwoBufQueueOptions options = {})
        : Base(writers, make_writer_record_callback<WriterT>(std::move(callback)), record_options(options)) {}

private:
    static TwoBufQueueOptions record_options(TwoBufQueueOptions options) {
        if (options.output_mode == HandoffMode::Sync)
            throw std::invalid_argument("producer-thread sync callbacks require a non-void Writer TSyncContext");
        return options;
    }
};

// 同步路径只持有原始回调 Context 与 Writer 借用，不构造输出池或任何读取线程。
// Writer 必须先 finish、写线程必须先 join，再 stop；此时仅归还原构造/finish 的空源块。
template<class RecordCallback, class WriterT>
    requires (is_sync_writer<WriterT>::value)
class TwoBufQueue<RecordCallback, WriterT> {
    static_assert(is_sync_writer<WriterT>::value, "sync selection must match the Writer type");
    using Context = typename WriterT::SyncContextType;
    static_assert(std::is_same_v<RecordCallback, typename Context::CallbackType>,
                  "queue callback must match the SyncContext callback type");
    static_assert([] {
        if constexpr (is_seq_writer<WriterT>::value)
            return Context::template accepts_record<std::int32_t,
                typename WriterT::SeqType, const std::byte*, std::size_t>;
        else
            return Context::template accepts_record<std::int32_t,
                const std::byte*, std::size_t>;
    }(), "sync callback must use the ordinary / SeqWriter noexcept record signature");

public:
    TwoBufQueue(RecordCallback callback, WriterT& writer, TwoBufQueueOptions = {})
        : context_(std::move(callback), false), writers_(&writer) {
        validate_writer(writer);
        writer.set_sync_callback(context_);
    }

    TwoBufQueue(RecordCallback callback, std::initializer_list<WriterT*> writers,
                TwoBufQueueOptions = {})
        : context_(std::move(callback), writers.size() > 1),
          writers_(std::in_place_type<std::vector<WriterT*>>, writers) {
        if (writers.size() == 0) throw std::invalid_argument("sync queue requires at least one writer");
        const auto& list = std::get<std::vector<WriterT*>>(writers_);
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
        for_each_writer([&](WriterT& writer) {
            writer.clear_sync_callback(context_);
            auto& source = writer.handoff_queue();
            if (source.current_producer()) {
                std::fputs("sync queue stop requires writer.finish() after stopping production\n", stderr);
                std::terminate();
            }
            source.close();
            while (auto record = source.try_pop()) {
                // 同步 write 不推进源游标，因此旧生命周期只有开始记录与空完成块。
                if (record.get() && !record->readable().empty()) {
                    std::fputs("sync writer unexpectedly contains buffered data\n", stderr);
                    std::terminate();
                }
                source.finish(record);
            }
            if (!source.is_drained()) std::terminate();
        });
    }

    // 同步模式 started 表示绑定可用；stats 的复制侧字段保持零。
    [[nodiscard]] bool started() const noexcept { return !stopped_; }
    [[nodiscard]] ReadError error_code() const noexcept { return ReadError::none; }
    [[nodiscard]] std::string last_error() const { return error_message(error_code()); }
    [[nodiscard]] ReadStats stats() const noexcept { return {}; }

private:
    static void validate_writer(const WriterT& writer) {
        if (writer.thread_id() < 0 || !writer.handoff_queue().current_producer() ||
            writer.handoff_queue().is_closed() || writer.handoff_queue().has_read_available_callback() ||
            writer.has_sync_callback())
            throw std::invalid_argument("sync writer must be live and exclusively bound to this queue");
    }
    template<class Function>
    void for_each_writer(Function function) noexcept {
        if (auto* single = std::get_if<WriterT*>(&writers_)) function(**single);
        else for (auto* writer : std::get<std::vector<WriterT*>>(writers_)) function(*writer);
    }

    Context context_;
    std::variant<WriterT*, std::vector<WriterT*>> writers_;
    bool stopped_ = false;
};

// 「双缓冲记录队列」的整批回调版本：回调收到整个 DataBatch（若干 frame 的只读视图），
// 返回 true 表示整批已处理；false 表示整批未处理，下一轮从头重试。
// 适用于无长度前缀的 frameless writer（如 Writer<1,false>），也适用于需要整批吞吐的场景。
template<class BatchCallback, class WriterT>
class TwoBufQueueBatch
    : public TwoBufQueueBase<WriterT,
          decltype(make_batch_callback(std::declval<BatchCallback>()))> {
    using Base = TwoBufQueueBase<WriterT,
        decltype(make_batch_callback(std::declval<BatchCallback>()))>;
    static_assert(!is_sync_writer<WriterT>::value,
                  "non-void TSyncContext supports per-record TwoBufQueue callbacks, not DataBatch callbacks");

public:
    TwoBufQueueBatch(BatchCallback callback, WriterT& writer, TwoBufQueueOptions options = {})
        : Base(writer, make_batch_callback(std::move(callback)), options) {}

    TwoBufQueueBatch(BatchCallback callback, std::initializer_list<WriterT*> writers,
                      TwoBufQueueOptions options = {})
        : Base(writers, make_batch_callback(std::move(callback)), options) {}
};

// 类模板实参推导：WriterT 从传入的 writer 自动推导（普通 Writer 或 SeqWriter 均可）。
template<class RecordCallback, class WriterT>
TwoBufQueue(RecordCallback, WriterT&, TwoBufQueueOptions = {}) -> TwoBufQueue<RecordCallback, WriterT>;

template<class RecordCallback, class WriterT>
TwoBufQueue(RecordCallback, std::initializer_list<WriterT*>, TwoBufQueueOptions = {}) -> TwoBufQueue<RecordCallback, WriterT>;

template<class BatchCallback, class WriterT>
TwoBufQueueBatch(BatchCallback, WriterT&, TwoBufQueueOptions = {}) -> TwoBufQueueBatch<BatchCallback, WriterT>;

template<class BatchCallback, class WriterT>
TwoBufQueueBatch(BatchCallback, std::initializer_list<WriterT*>, TwoBufQueueOptions = {}) -> TwoBufQueueBatch<BatchCallback, WriterT>;

} // namespace TwoBufQueue
