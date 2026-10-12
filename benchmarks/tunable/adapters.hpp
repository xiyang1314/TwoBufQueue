#pragma once
#include "config.hpp"
#include <rigtorp/SPSCQueue.h>
#include <readerwriterqueue.h>
#pragma push_macro("nil")
#undef nil // macOS MacTypes defines nil; isolate the unmodified upstream header.
#include <atomic_queue/atomic_queue.h>
#pragma pop_macro("nil")
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace TwoBufQueue::performance {
struct PressureSnapshot {
    bool available = false;
    WriterBackpressureStats writer{};
    ReadStats copy{};
};
#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
struct PipelinePoolDepthStats {
    PoolDepthStats writer, reader;
};
#endif
// Ordinary copying remains one complete member copy. Original try_emplace APIs
// can request rigtorp write prefetch or ReaderWriterQueue zero-then-copy.
// Both optional operations and the complete payload copy are timed and recorded.
inline constexpr char payload_copy_strategy[] = "member_copy_rigtorp_prefetch_readerwriterqueue_optional_zero";
struct DestinationWritePrefetch {};
struct CopyAfterZero {};
template<std::size_t TBytes> struct Payload {
    static_assert(TBytes % 8 == 0);
    std::array<std::byte,TBytes> bytes{};
    Payload() = default;
    // Copy-initialize the complete payload; a body memcpy first applies bytes{} zeroing.
    Payload(const Payload& other) : bytes(other.bytes) {}
    // bytes{} initializes the queue slot before this one complete payload copy.
    Payload(const Payload& other, CopyAfterZero) {
        std::memcpy(bytes.data(),other.bytes.data(),TBytes);
    }
    static const std::array<std::byte,TBytes>& copy_source(
            const Payload& other, const void* destination) noexcept {
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(destination,1,3);
#else
        (void)destination;
#endif
        return other.bytes;
    }
    Payload(const Payload& other, DestinationWritePrefetch)
        : bytes(copy_source(other,this)) {}
    Payload& operator=(const Payload& other) {
        if (this != &other)
            std::memcpy(bytes.data(),other.bytes.data(),TBytes);
        return *this;
    }
};

template<class TPayload, bool TWritePrefetch = true> class RigtorpAdapter {
    rigtorp::SPSCQueue<TPayload> queue_;
public:
    using Value = TPayload;
    explicit RigtorpAdapter(std::size_t capacity) : queue_(capacity) {}
    bool push(const TPayload& payload) noexcept {
        if constexpr (TWritePrefetch)
            return queue_.try_emplace(payload,DestinationWritePrefetch{});
        else
            return queue_.try_push(payload);
    }
    template<class TSink> bool consume(TSink& sink) noexcept {
        auto* value = queue_.front(); if (!value) return false;
        sink(0,value->bytes.data(),value->bytes.size()); queue_.pop(); return true;
    }
    std::size_t capacity() const noexcept { return queue_.capacity(); }
};
template<class TPayload, bool TCopyAfterZero = false> class ReaderWriterAdapter {
    moodycamel::ReaderWriterQueue<TPayload> queue_;
public:
    using Value = TPayload;
    explicit ReaderWriterAdapter(std::size_t capacity) : queue_(capacity) {}
    bool push(const TPayload& payload) noexcept {
        if constexpr (TCopyAfterZero)
            return queue_.try_emplace(payload,CopyAfterZero{});
        else
            return queue_.try_enqueue(payload);
    }
    template<class TSink> bool consume(TSink& sink) noexcept {
        auto* value = queue_.peek(); if (!value) return false;
        sink(0,value->bytes.data(),value->bytes.size()); (void)queue_.pop(); return true;
    }
    std::size_t capacity() const noexcept { return queue_.max_capacity(); }
};
template<class TPayload> class AtomicAdapter {
    // 最后一个模板参数明确启用 SPSC；TPayload 为非原子完整消息。
    atomic_queue::AtomicQueueB2<TPayload,std::allocator<TPayload>,true,false,true> queue_;
public:
    using Value = TPayload;
    explicit AtomicAdapter(std::size_t capacity) : queue_(static_cast<unsigned>(capacity)) {}
    bool push(const TPayload& payload) noexcept { return queue_.try_push(payload); }
    template<class TSink> bool consume(TSink& sink) noexcept {
        TPayload value; if (!queue_.try_pop(value)) return false;
        sink(0,value.bytes.data(),value.bytes.size()); return true;
    }
    std::size_t capacity() const noexcept { return queue_.capacity(); }
};

template<class TSink, class TAdapter>
class SpscChannel {
public:
    SpscChannel(const PerformanceConfig& config, TSink& sink)
        : config_(config), queue_(config.requested_capacity()) {
        if (config.prefault_producer_buffers) {
            typename TAdapter::Value value{};
            while (queue_.push(value)) {}
            auto discard = [](std::int32_t,const std::byte*,std::size_t) noexcept {};
            while (queue_.consume(discard)) {}
        }
        reader_ = std::thread([this,&sink] {
            affinity_ok_ = pin_current_thread(config_.output.read_config.worker_cpu);
            ready_.store(true,std::memory_order_release);
            std::uint32_t idle = 0;
            while (!stop_.load(std::memory_order_acquire)) {
                if (queue_.consume(sink)) { idle = 0; continue; }
                if (idle < config_.spsc_idle_spins) { ++idle; cpu_relax(); }
                else if (config_.spsc_idle_sleep_ns) {
                    std::this_thread::sleep_for(std::chrono::nanoseconds{config_.spsc_idle_sleep_ns}); idle = 0;
                } else { std::this_thread::yield(); idle = 0; }
            }
        });
        while (!ready_.load(std::memory_order_acquire)) std::this_thread::yield();
        if (!affinity_ok_) { stop(); throw std::runtime_error("consumer CPU affinity unavailable/invalid"); }
    }
    ~SpscChannel() { stop(); }
    template<class TPayload> bool push(const TPayload& payload) noexcept { return queue_.push(payload); }
    void finish() noexcept {}
    void finish_block() noexcept {} // 原生 SPSC 每条已发布，轮末无需额外交接。
    void stop() noexcept {
        stop_.store(true,std::memory_order_release);
        if (reader_.joinable()) reader_.join();
    }
    void check() const {}
    std::size_t capacity() const noexcept { return queue_.capacity(); }
    PressureSnapshot pressure_stats() const noexcept { return {}; }
private:
    const PerformanceConfig& config_;
    TAdapter queue_;
    std::atomic<bool> ready_{false}, stop_{false};
    bool affinity_ok_ = true;
    std::thread reader_;
};

// Frames contain fixed-size records, with no length prefix. Validate every
// frame before delivering callbacks, so malformed batches never replay a prefix.
template<std::size_t TBytes, class TSink> struct FixedRecordCallback {
    TSink* sink;
    bool operator()(const DataBatch& batch) const noexcept {
        for (const auto& frame : batch.frames)
            if (frame.size % TBytes != 0) return false;
        for (const auto& frame : batch.frames)
            for (std::size_t offset = 0; offset < frame.size; offset += TBytes)
                (*sink)(frame.thread_id, frame.data + offset, TBytes);
        return true;
    }
};
template<std::uint32_t TPublish, class TSink, std::size_t TBytes = 8>
class TwoBufChannel {
    using WriterType = OwnedWriter<Writer<TPublish, false, std::uint16_t, void>>;
    static_assert(WriterType::message_prefix_size == 0);
    const std::chrono::milliseconds session_timeout_;
    WriterType writer_;
    ::TwoBufQueue::TwoBufQueueBatch<FixedRecordCallback<TBytes,TSink>,WriterType> pipeline_;
    static const TwoBufQueueOptions& prepare(WriterType& writer, const PerformanceConfig& config) {
        if (config.payload_bytes != TBytes)
            throw std::invalid_argument("channel record size differs from configured payload size");
        if (config.prefault_producer_buffers) {
            // 消费线程启动前，持有全部预分配块以避免 LIFO 池重复触碰同一块。
            std::vector<SpscWriteBlock*> blocks;
            blocks.reserve(config.source.pool_min);
            if (auto* current = writer.source_block()) std::memset(current->data(),0,current->capacity());
            for (std::size_t i = 1; i < config.source.pool_min; ++i) {
                auto loan = writer.handoff_queue().pool().get();
                if (!loan) {
                    for (auto* block : blocks) {
                        (void)block->to_write_finished(); (void)block->to_leave_writer(); (void)block->finish();
                    }
                    throw std::runtime_error("cannot prefault source pool");
                }
                auto* block = loan.get();
                blocks.push_back(block);
                std::memset(block->data(),0,block->capacity());
            }
            for (auto* block : blocks) {
                if (!block->to_write_finished() || !block->to_leave_writer() || !block->finish())
                    throw std::runtime_error("cannot recycle prefaulted source block");
            }
        }
        return config.output;
    }
public:
    static constexpr std::size_t message_prefix_size = WriterType::message_prefix_size;
    TwoBufChannel(const PerformanceConfig& config, TSink& sink)
        : session_timeout_(config.source.backpressure.wait_get_timeout),
          writer_(config.source), pipeline_(FixedRecordCallback<TBytes,TSink>{&sink},writer_,prepare(writer_,config)) {
        if (!pipeline_.started()) { writer_.finish(); pipeline_.stop(); throw std::runtime_error(pipeline_.last_error()); }
    }
    ~TwoBufChannel() { stop(); }
    // Use Writer's normal fixed-size write API without a length prefix.
    template<class TPayload> bool push(const TPayload& payload) noexcept { return writer_.write(payload.bytes); }
    struct Session {
        typename WriterType::WriteSession writer_session;
        explicit operator bool() const noexcept { return static_cast<bool>(writer_session); }
        template<class TPayload> FAST_LOG_ALWAYS_INLINE SessionWriteResult push(const TPayload& payload) noexcept {
            return writer_session.write(payload.bytes);
        }
    };
    [[nodiscard]] Session get_current_session() requires(WriterType::session_enabled) {
        return Session{writer_.get_current_session(session_timeout_)};
    }
    [[nodiscard]] Session get_next_session() requires(WriterType::session_enabled) {
        return Session{writer_.get_next_session(session_timeout_)};
    }
    void finish() noexcept { writer_.finish(); }
    void finish_block() noexcept { writer_.finish_block(); }
    void stop() noexcept { writer_.finish(); pipeline_.stop(); }
    void check() const {
        if (pipeline_.error_code() != ReadError::none) throw std::runtime_error(pipeline_.last_error());
    }
    PressureSnapshot pressure_stats() const noexcept {
        return {true,writer_.backpressure_stats(),pipeline_.stats()};
    }
#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
    void begin_pool_depth_stats() noexcept {
        writer_.handoff_queue().pool().begin_depth_stats();
        pipeline_.begin_output_pool_depth_stats();
    }
    PipelinePoolDepthStats end_pool_depth_stats() noexcept {
        return {writer_.handoff_queue().pool().end_depth_stats(),
                pipeline_.end_output_pool_depth_stats()};
    }
#endif
    std::size_t capacity() const noexcept { return 0; } // 源块池及输出池分别报告，不伪装成单个环形队列容量。
};
} // namespace TwoBufQueue::performance
