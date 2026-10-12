#pragma once
#include "adapters.hpp"
#include "timing.hpp"
#include "publication_plan.hpp"
#include "../process_metrics.hpp"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <optional>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace TwoBufQueue::performance {
struct PerformanceResult {
    PerformanceConfig config;
#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
    bool pool_depth_stats_available = false;
    PipelinePoolDepthStats pool_depth{};
#endif
    double seconds = 0, m_messages_per_second = 0, payload_gb_per_second = 0;
    double producer_seconds = 0, wall_seconds = 0, drain_seconds = 0;
    double input_preparation_seconds = 0;
    std::size_t prepared_input_bytes = 0;
    double callback_processing_seconds = 0, callback_to_producer_ratio = 0;
    std::uint64_t timed_callback_messages = 0;
    std::uint64_t completed_rounds = 0, round_message_limit = 0, round_message_minimum = 0;
    std::uint64_t effective_warmup_messages = 0;
    std::uint32_t publication_alignment = 1;
    std::size_t source_record_bytes = 0;
    bool producer_hard_affinity = false, producer_qos_applied = false;
    std::uint32_t producer_qos_actual = 0;
    double cpu_cores = 0, peak_rss_mib = 0;
    std::uint64_t consumed = 0, failed_write_calls = 0, callback_work_checksum = 0;
    std::uint64_t session_block_exhaustions = 0; // 普通换块边界，不算背压或失败消息。
    // Formal writes only; retry clocks run solely after a failed nonblocking push.
    std::uint64_t backpressured_messages = 0;
    double producer_retry_seconds = 0, producer_retry_max_seconds = 0;
    bool library_pressure_stats_available = false;
    std::uint64_t writer_pressure_events = 0, writer_rejected_attempts = 0, writer_blocked_events = 0;
    std::uint64_t writer_blocked_ticks = 0;
    std::uint64_t copy_memory_full_events = 0, copy_backpressured_frames = 0;
    std::size_t actual_spsc_capacity = 0, nominal_buffer_bytes = 0;
    std::size_t latency_samples = 0, failed_latency_samples = 0, migrated_samples = 0;
    std::string timer_name;
    double timer_quantum_ns = 0, latency_quantum_ns = 0;
    std::uint64_t latency_tail_writes = 0;
    Quantiles timer_overhead_ns{}, successful_write_ns{}, failed_write_ns{}, batch_average_write_ns{};
    bool resolution_limited = false;
    double call_batch_mean_ns = 0, call_batch_empty_mean_ns = 0;
    std::uint64_t call_batch_attempts = 0;
};
namespace detail {
// Called only after the first failed attempt; no clocks on successful writes.
template<class TAttempt, class TPause>
FAST_LOG_ALWAYS_INLINE void retry_failed_write(PerformanceResult& result, TAttempt attempt, TPause pause) {
    const auto begin = std::chrono::steady_clock::now();
    ++result.backpressured_messages;
    do { ++result.failed_write_calls; pause(); } while (!attempt());
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    result.producer_retry_seconds += elapsed;
    result.producer_retry_max_seconds = std::max(result.producer_retry_max_seconds,elapsed);
}
// 只在控制路径运行；避免坏配置或死锁让搜索永久卡住。
class Watchdog {
    std::mutex mutex_;
    std::condition_variable event_;
    bool stopped_ = false;
    std::thread thread_;
public:
    explicit Watchdog(std::uint32_t seconds) : thread_([this,seconds] {
        std::unique_lock lock(mutex_);
        if (!event_.wait_for(lock,std::chrono::seconds{seconds},[this] { return stopped_; })) {
            std::fputs("performance test timed out\n",stderr); std::abort();
        }
    }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex_); stopped_ = true; }
        event_.notify_one(); thread_.join();
    }
};
class ProducerAffinity {
    bool changed_ = false;
#if defined(__linux__)
    cpu_set_t previous_{};
#elif defined(_WIN32)
    DWORD_PTR previous_ = 0;
#endif
public:
    explicit ProducerAffinity(std::int32_t cpu) {
        if (cpu < 0) return;
#if defined(__linux__)
        if (pthread_getaffinity_np(pthread_self(),sizeof(previous_),&previous_))
            throw std::runtime_error("cannot read producer affinity");
        changed_ = pin_current_thread(cpu);
#elif defined(_WIN32)
        if (cpu < static_cast<std::int32_t>(sizeof(DWORD_PTR)*8))
            previous_ = SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR{1} << cpu);
        changed_ = previous_ != 0;
#endif
        if (!changed_) throw std::runtime_error("producer CPU affinity unavailable/invalid on this platform");
    }
    ~ProducerAffinity() {
#if defined(__linux__)
        if (changed_) (void)pthread_setaffinity_np(pthread_self(),sizeof(previous_),&previous_);
#elif defined(_WIN32)
        if (changed_) (void)SetThreadAffinityMask(GetCurrentThread(),previous_);
#endif
    }
};
class ProducerScheduling {
#if defined(__APPLE__)
    qos_class_t previous_ = QOS_CLASS_UNSPECIFIED;
    int priority_ = 0;
#endif
    bool changed_ = false;
public:
    ProducerScheduling(ProducerQos qos, PerformanceResult& result) {
        if (qos == ProducerQos::unchanged) return;
#if defined(__APPLE__)
        if (pthread_get_qos_class_np(pthread_self(),&previous_,&priority_))
            throw std::runtime_error("cannot read producer QoS");
        // macOS setter 不接受 UNSPECIFIED；此时结束后回到 DEFAULT，避免遗留高 QoS。
        if (previous_ == QOS_CLASS_UNSPECIFIED) { previous_ = QOS_CLASS_DEFAULT; priority_ = 0; }
        if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0))
            throw std::runtime_error("cannot set producer high QoS");
        changed_ = true;
        qos_class_t actual; int relative;
        if (pthread_get_qos_class_np(pthread_self(),&actual,&relative) || actual != QOS_CLASS_USER_INTERACTIVE) {
            (void)pthread_set_qos_class_self_np(previous_,priority_);
            throw std::runtime_error("producer high QoS verification failed");
        }
        result.producer_qos_applied = true;
        result.producer_qos_actual = static_cast<std::uint32_t>(actual);
#else
        (void)result;
        throw std::runtime_error("user_interactive QoS is available only on macOS; use producer_cpu on Linux/Windows");
#endif
    }
    ~ProducerScheduling() {
#if defined(__APPLE__)
        if (changed_) (void)pthread_set_qos_class_self_np(previous_,priority_);
#endif
    }
};
inline void observable(std::uint64_t& value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : "+r"(value) :: "memory");
#else
    // MSVC: volatile 防止消除模拟循环，不用于线程同步。
    volatile std::uint64_t retained = value; value = retained;
#endif
}
inline std::uint64_t payload_word(std::uint64_t sequence, std::size_t word) noexcept {
    auto value = sequence + 0x9e3779b97f4a7c15ULL*word;
    value = (value ^ (value >> 30))*0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27))*0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}
template<std::size_t TBytes>
void set_payload(Payload<TBytes>& payload, std::uint64_t sequence, bool variable) noexcept {
    std::memcpy(payload.bytes.data(),&sequence,8);
    if (variable) for (std::size_t word=1; word<TBytes/8; ++word) {
        const auto value=payload_word(sequence,word);
        std::memcpy(payload.bytes.data()+word*8,&value,8);
    }
}
template<std::size_t TBytes> struct Sink {
    const PerformanceConfig& config;
    const PublicationPlan& publication_plan;
    const std::uint64_t warmup, total;
    std::uint64_t received = 0, work_checksum = 0;
    bool valid = true;
    std::chrono::steady_clock::time_point finish_time{};
    alignas(64) std::atomic<bool> warmup_done{false}, done{false};
    alignas(64) std::atomic<std::uint64_t> completed_rounds{0};
    std::uint64_t next_round_end = warmup+publication_plan.next_end(0);
    std::uint64_t rounds = 0;
    std::uint64_t next_callback_begin = warmup, timed_callback_messages = 0;
    std::chrono::steady_clock::time_point callback_begin{};
    double callback_processing_seconds = 0;
    void operator()(std::int32_t source, const std::byte* ptr, std::size_t size) noexcept {
        if (config.measure_callback_time && received == next_callback_begin)
            callback_begin = std::chrono::steady_clock::now();
        if (size != TBytes || source != 0) valid = false;
        std::uint64_t sequence = 0;
        if (size >= sizeof(sequence)) std::memcpy(&sequence,ptr,sizeof(sequence));
        valid &= sequence == received;
        if (config.verify_payload && size == TBytes) {
            if (config.variable_payload) {
                for (std::size_t word=1; word<TBytes/8; ++word) {
                    std::uint64_t value=0;
                    std::memcpy(&value,ptr+word*8,8);
                    valid &= value == payload_word(received,word);
                }
            } else for (std::size_t i=8; i<TBytes; ++i) valid &= ptr[i] == std::byte{0xa5};
        }
        // 模拟业务循环每条消息执行一次；0 时跳过全部模拟计算。
        if (config.callback_iterations) {
            auto work = sequence ^ work_checksum;
            for (std::uint32_t i = 0; i < config.callback_iterations; ++i) {
                work ^= work << 13; work ^= work >> 7; work ^= work << 17;
                work += 0x9e3779b97f4a7c15ULL; observable(work);
            }
            work_checksum = work;
        }
        ++received;
        if (received == warmup) warmup_done.store(true,std::memory_order_release);
        // 仅读线程维护 received/边界；每轮最后一条回调完成才发布一次，避免逐条原子写。
        if (config.round_storage_bytes && received == next_round_end) {
            if (config.measure_callback_time) {
                callback_processing_seconds += std::chrono::duration<double>(
                    std::chrono::steady_clock::now()-callback_begin).count();
                timed_callback_messages += received-next_callback_begin;
                next_callback_begin = received;
            }
            next_round_end = warmup+publication_plan.next_end(received-warmup);
            completed_rounds.store(++rounds,std::memory_order_release);
        }
        if (received == total) {
            finish_time = std::chrono::steady_clock::now(); done.store(true,std::memory_order_release);
        }
    }
};
inline std::uint64_t random_next(std::uint64_t& state) noexcept {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state;
}
} // namespace detail

// 可嵌入用户自己的程序；配置在构造函数传入，run 每次构造全新队列/线程。
// 热路径使用模板分派，避免 std::function / 虚函数成为比较双方的共同瓶颈。
class PerformanceTest {
public:
    explicit PerformanceTest(PerformanceConfig config) : config_(std::move(config)) { config_.validate(); }
    [[nodiscard]] const PerformanceConfig& config() const noexcept { return config_; }
    [[nodiscard]] PerformanceResult run() const {
#if defined(BRQ_BLOCK_SIZE_ONLY)
        if (config_.transport != Transport::two_buf_queue || config_.payload_bytes != 32 || config_.use_write_session)
            throw std::invalid_argument("block-size test requires ordinary 32 B writes");
        if (config_.publish_count == 1) return measure<32,TwoBufChannel<1,detail::Sink<32>,32>>();
        if (config_.publish_count == (std::numeric_limits<std::uint32_t>::max)())
            return measure<32,TwoBufChannel<(std::numeric_limits<std::uint32_t>::max)(),detail::Sink<32>,32>>();
        throw std::invalid_argument("block-size test requires publish_count=1 or UINT32_MAX");
#else
        switch (config_.payload_bytes) {
        case 8: return run_payload<8>(); case 16: return run_payload<16>();
        case 32: return run_payload<32>(); case 64: return run_payload<64>();
        }
        throw std::invalid_argument("unsupported payload size");
#endif
    }
private:
    template<std::size_t TBytes> PerformanceResult run_payload() const {
        using Value = Payload<TBytes>; using Sink = detail::Sink<TBytes>;
        switch (config_.transport) {
        case Transport::two_buf_queue:
            switch (config_.publish_count) {
            case 1: return measure<TBytes,TwoBufChannel<1,Sink,TBytes>>();
            case 2: return measure<TBytes,TwoBufChannel<2,Sink,TBytes>>();
            case 4: return measure<TBytes,TwoBufChannel<4,Sink,TBytes>>();
            case 8: return measure<TBytes,TwoBufChannel<8,Sink,TBytes>>();
            case 10: return measure<TBytes,TwoBufChannel<10,Sink,TBytes>>();
            case 16: return measure<TBytes,TwoBufChannel<16,Sink,TBytes>>();
            case 32: return measure<TBytes,TwoBufChannel<32,Sink,TBytes>>();
            case 64: return measure<TBytes,TwoBufChannel<64,Sink,TBytes>>();
            case 128: return measure<TBytes,TwoBufChannel<128,Sink,TBytes>>();
            case 256: return measure<TBytes,TwoBufChannel<256,Sink,TBytes>>();
            case 512: return measure<TBytes,TwoBufChannel<512,Sink,TBytes>>();
            case 1024: return measure<TBytes,TwoBufChannel<1024,Sink,TBytes>>();
            case (std::numeric_limits<std::uint32_t>::max)():
                return measure<TBytes,TwoBufChannel<(std::numeric_limits<std::uint32_t>::max)(),Sink,TBytes>>();
            }
            break;
        case Transport::rigtorp:
            if (config_.rigtorp_write_prefetch)
                return measure<TBytes,SpscChannel<Sink,RigtorpAdapter<Value,true>>>();
            else
                return measure<TBytes,SpscChannel<Sink,RigtorpAdapter<Value,false>>>();
        case Transport::readerwriterqueue:
            if (config_.readerwriterqueue_copy_after_zero)
                return measure<TBytes,SpscChannel<Sink,ReaderWriterAdapter<Value,true>>>();
            else
                return measure<TBytes,SpscChannel<Sink,ReaderWriterAdapter<Value,false>>>();
        case Transport::atomic_queue: return measure<TBytes,SpscChannel<Sink,AtomicAdapter<Value>>>();
        }
        throw std::invalid_argument("unsupported transport/publish count");
    }
    template<std::size_t TBytes, class TChannel> PerformanceResult measure() const {
        detail::Watchdog watchdog(config_.timeout_seconds);
        if (config_.transport != Transport::two_buf_queue &&
            (!config_.requested_capacity() || config_.requested_capacity() > (std::numeric_limits<unsigned>::max)()/2 ||
             config_.requested_capacity() > config_.buffer_budget_bytes/(2*TBytes)))
            throw std::invalid_argument("SPSC capacity exceeds buffer budget or integer range");
        const PublicationPlan publication_plan(config_);
        const auto warmup = publication_plan.warmup;
        detail::Sink<TBytes> sink{config_,publication_plan,warmup,warmup+config_.messages};
        PerformanceResult result; result.config = config_;
        std::optional<ProbeClock> clock;
        std::vector<double> success_samples, failure_samples;
        const auto sample_limit = std::min<std::uint64_t>(config_.mode == MeasureMode::write_latency
            ? config_.messages/config_.latency_batch_size : config_.messages,config_.max_samples);
        std::vector<Payload<TBytes>> batch;
        if (config_.mode != MeasureMode::throughput) {
            clock.emplace(config_);
            result.timer_name = clock->label(); result.timer_quantum_ns = clock->quantum_ns();
            result.timer_overhead_ns = clock->overhead();
            result.latency_quantum_ns = result.timer_quantum_ns /
                (config_.mode == MeasureMode::write_latency ? config_.latency_batch_size : 1);
            if (config_.mode == MeasureMode::single_write_latency || config_.mode == MeasureMode::write_latency) {
                success_samples.reserve(sample_limit);
                failure_samples.reserve(sample_limit);
                if (config_.mode == MeasureMode::write_latency) {
                    batch.resize(config_.latency_batch_size);
                    for (auto& value : batch) value.bytes.fill(std::byte{0xa5});
                }
            } else {
                batch.resize(config_.call_batch_size);
                for (auto& value : batch) value.bytes.fill(std::byte{0xa5});
                // 同样的输入访问/分支循环参考值；不从单次调用分位数中相减。
                double elapsed = 0; std::uint64_t kept = 0;
                for (unsigned repeat = 0; repeat < 1000; ++repeat) {
                    const auto begin = clock->read();
                    for (std::size_t k = 0; k < batch.size(); ++k) { kept += batch[k].bytes.size(); detail::observable(kept); }
                    const auto end = clock->read();
                    elapsed += clock->elapsed(begin,end);
                }
                result.call_batch_empty_mean_ns = elapsed/(1000.0*batch.size());
                detail::observable(kept);
            }
        }
        TChannel channel(config_,sink);
        // 在消费/复制/看门狗线程创建后绑写线程，避免新线程继承写线程的亲和性。
        detail::ProducerAffinity affinity(config_.producer_cpu);
        detail::ProducerScheduling scheduling(config_.producer_qos,result);
        result.producer_hard_affinity = config_.producer_cpu >= 0;
        result.round_message_limit = publication_plan.target_records;
        result.round_message_minimum = publication_plan.minimum_round_records();
        result.effective_warmup_messages = warmup;
        result.publication_alignment = publication_plan.alignment;
        result.source_record_bytes = TBytes;
        result.actual_spsc_capacity = channel.capacity();
        result.nominal_buffer_bytes = config_.transport == Transport::two_buf_queue
            ? config_.source.block_size*config_.source.pool_max + config_.output.output_block_size*config_.output.output_pool_max
            : result.actual_spsc_capacity*TBytes;
        if (result.nominal_buffer_bytes > config_.buffer_budget_bytes) throw std::invalid_argument("rounded capacity exceeds buffer budget");
        Payload<TBytes> payload; payload.bytes.fill(std::byte{0xa5});
        std::uint64_t retry = 0;
        const auto pause = [&] {
            ++retry;
            if (config_.retry_yield_every && retry % config_.retry_yield_every == 0) std::this_thread::yield();
            else cpu_relax();
        };
        for (std::uint64_t i = 0; i < warmup; ++i) {
            detail::set_payload(payload,i,config_.variable_payload);
            while (!channel.push(payload)) pause();
        }
        if (publication_plan.finish_blocks) channel.finish_block();
        while (!sink.warmup_done.load(std::memory_order_acquire)) std::this_thread::yield();
        channel.check();
        retry = 0;
        const auto pressure_start = channel.pressure_stats();
        // 让编译器决定测试模式分派与整轮发送循环的内联边界。
        // 强制展开会把普通写入、Session、计时逻辑并入大函数，改变热循环的生成代码。
        // Writer::write 的内联属性保留；计时内仍逐条调用队列的写入接口。
        const auto retry_until_accepted = [&](auto attempt) {
            detail::retry_failed_write(result,attempt,pause);
        };
        std::vector<Payload<TBytes>> prepared;
        const auto prepare_round = [&](std::uint64_t begin, std::uint64_t end) {
            if (!config_.prepared_input) return;
            const auto prepare_begin = std::chrono::steady_clock::now();
            prepared.resize(static_cast<std::size_t>(end-begin));
            for (std::size_t k=0; k<prepared.size(); ++k) {
                prepared[k].bytes.fill(std::byte{0xa5});
                detail::set_payload(prepared[k],warmup+begin+k,config_.variable_payload);
            }
            result.prepared_input_bytes=std::max(result.prepared_input_bytes,prepared.size()*TBytes);
            result.input_preparation_seconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now()-prepare_begin).count();
        };
        prepare_round(0,publication_plan.next_end(0));
#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
        if constexpr (requires { channel.begin_pool_depth_stats(); }) channel.begin_pool_depth_stats();
#endif
        const double cpu_start = benchmark::cpu_seconds();
        const auto start = std::chrono::steady_clock::now();
        auto next_due = start;
        std::uint64_t random_state = config_.random_seed ? config_.random_seed : 1;
        std::uint64_t next_sample = detail::random_next(random_state)%config_.sample_every;
        double batch_elapsed = 0;
        std::uint64_t round_begin = 0;
        while (round_begin < config_.messages) {
        const auto round_end = publication_plan.next_end(round_begin);
        if (round_begin) prepare_round(round_begin,round_end);
        const auto active_start = std::chrono::steady_clock::now();
        if (config_.mode == MeasureMode::write_latency) {
            const auto send_latency = [&](auto& destination) {
                std::uint64_t sent = round_begin;
                while (sent < round_end) {
                    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(batch.size(),round_end-sent));
                    if (!config_.prepared_input) for (std::size_t k=0; k<count; ++k)
                        detail::set_payload(batch[k],warmup+sent+k,config_.variable_payload);
                    const auto* input=config_.prepared_input ? prepared.data()+(sent-round_begin) : batch.data();
                    const bool sampled = count == config_.latency_batch_size && success_samples.size() < sample_limit;
                    ProbeClock::Stamp begin{};
                    if (sampled) begin = clock->read();
                    for (std::size_t k = 0; k < count; ++k) {
                        if (!destination.push(input[k]))
                            retry_until_accepted([&] { return destination.push(input[k]); });
                    }
                    if (sampled) {
                        const auto end = clock->read();
                        if (begin.cpu != end.cpu || end.ticks < begin.ticks) ++result.migrated_samples;
                        else success_samples.push_back(clock->elapsed(begin,end)/count);
                    }
                    sent += count;
                    if (count != config_.latency_batch_size) result.latency_tail_writes += count;
                }
            };
            if constexpr (requires { channel.get_current_session(); }) {
                if (config_.use_write_session) {
                    // Session 跨计时组保留；组中遇到块满时，计时继续覆盖获取下一块。
                    auto* input = prepared.data();
                    const auto* finish = input + (round_end - round_begin);
                    const decltype(finish) no_group = nullptr;
                    decltype(finish) group_end = no_group;
                    std::size_t group_count = 0;
                    bool sampled = false, first = true;
                    ProbeClock::Stamp begin{};
                    const auto begin_group = [&]() {
                        group_count = std::min<std::size_t>(config_.latency_batch_size, finish - input);
                        group_end = input + group_count;
                        sampled = group_count == config_.latency_batch_size && success_samples.size() < sample_limit;
                        if (sampled) begin = clock->read();
                    };
                    while (input != finish) {
                        if (!group_end) begin_group(); // 首次获取 Session 也包含在本组计时中。
                        auto local = first ? channel.get_current_session() : channel.get_next_session();
                        first = false;
                        if (!local) throw std::runtime_error("session acquisition failed or timed out");
                        bool block_full = false;
                        while (input != finish) {
                            if (!group_end) begin_group();
                            for (; input != group_end; ++input) {
                                const auto status = local.push(*input);
                                if (status == SessionWriteResult::insufficient_space) {
                                    ++result.session_block_exhaustions;
                                    block_full = true;
                                    break;
                                }
                                if (status != SessionWriteResult::success)
                                    throw std::runtime_error("session record write error");
                            }
                            if (block_full) break; // 不推进 input；外层负责获取下一块。
                            if (sampled) {
                                const auto end = clock->read();
                                if (begin.cpu != end.cpu || end.ticks < begin.ticks) ++result.migrated_samples;
                                else success_samples.push_back(clock->elapsed(begin,end)/group_count);
                            }
                            if (group_count != config_.latency_batch_size) result.latency_tail_writes += group_count;
                            group_end = no_group;
                        }
                    }
                } else send_latency(channel);
            } else send_latency(channel);
        } else if (config_.mode == MeasureMode::call_batch) {
            std::uint64_t sent = 0;
            while (sent < config_.messages) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(batch.size(),config_.messages-sent));
                if (!config_.prepared_input) for (std::size_t k=0; k<count; ++k)
                    detail::set_payload(batch[k],warmup+sent+k,config_.variable_payload);
                const auto* input=config_.prepared_input ? prepared.data()+sent : batch.data();
                std::size_t accepted = 0; bool failed = false;
                const auto begin = clock->read();
                for (; accepted < count; ++accepted)
                    if (!channel.push(input[accepted])) { failed = true; break; }
                const auto end = clock->read();
                if (begin.cpu == end.cpu && end.ticks >= begin.ticks) {
                    batch_elapsed += clock->elapsed(begin,end);
                    result.call_batch_attempts += accepted+static_cast<unsigned>(failed);
                } else ++result.migrated_samples;
                sent += accepted;
                if (failed) {
                    retry_until_accepted([&] { return channel.push(input[accepted]); });
                    ++sent;
                }
            }
        } else {
            const auto produce = [&]<bool TSample, bool TPrepared, bool TPaced>() {
                // Copy invariants before the loop: byte writes and the rare
                // notification path otherwise inhibit alias-based hoisting.
                const auto first = round_begin, limit = round_end;
                const auto* const values = prepared.data();
                auto* const destination = &channel;
                const auto interval = config_.producer_interval_ns;
                const bool variable = config_.variable_payload;
                if constexpr (!TSample && TPrepared && !TPaced) {
                    const auto send_prepared = [&](auto& local) {
                        const auto* const finish = values+(limit-first);
                        for (auto* input = values; input != finish; ++input) {
                            if (!local.push(*input))
                                retry_until_accepted([&local,input] { return local.push(*input); });
                        }
                    };
                    if constexpr (requires { destination->get_current_session(); }) {
                        if (config_.use_write_session) {
                            const auto* input = values;
                            const auto* finish = values + (limit - first);
                            bool current = true;
                            while (input != finish) {
                                auto local = current ? destination->get_current_session() : destination->get_next_session();
                                current = false;
                                if (!local) throw std::runtime_error("session acquisition failed or timed out");
                                for (; input != finish; ++input) {
                                    const auto status = local.push(*input);
                                    if (status == SessionWriteResult::insufficient_space) {
                                        ++result.session_block_exhaustions;
                                        break;
                                    }
                                    if (status != SessionWriteResult::success)
                                        throw std::runtime_error("session record write error");
                                }
                            }
                        } else send_prepared(*destination);
                    } else send_prepared(*destination);
                } else {
                for (std::uint64_t i = first; i < limit; ++i) {
                    if constexpr (!TPrepared) detail::set_payload(payload,warmup+i,variable);
                    const auto& input = TPrepared ? values[i-first] : payload;
                    if constexpr (TPaced) {
                        next_due += std::chrono::nanoseconds{interval};
                        while (std::chrono::steady_clock::now() < next_due) cpu_relax();
                    }
                    const bool sampled = TSample && i == next_sample && success_samples.size() < sample_limit;
                    const auto attempt = [&] {
                        if constexpr (TSample) {
                            if (sampled) {
                                const auto begin = clock->read();
                                const bool accepted = destination->push(input); // one write / try_push / try_enqueue
                                const auto end = clock->read();
                                if (begin.cpu != end.cpu || end.ticks < begin.ticks) ++result.migrated_samples;
                                else if (accepted) success_samples.push_back(clock->elapsed(begin,end));
                                else if (failure_samples.size() < sample_limit) failure_samples.push_back(clock->elapsed(begin,end));
                                return accepted;
                            }
                        }
                        return destination->push(input);
                    };
                    if (!attempt()) retry_until_accepted(attempt);
                    if (sampled) next_sample += 1+detail::random_next(random_state)%(2ULL*config_.sample_every-1);
                }
                }
            };
            const auto dispatch = [&]<bool TPaced>() {
                if (config_.mode == MeasureMode::single_write_latency) {
                    if (config_.prepared_input) produce.template operator()<true,true,TPaced>();
                    else produce.template operator()<true,false,TPaced>();
                } else {
                    if (config_.prepared_input) produce.template operator()<false,true,TPaced>();
                    else produce.template operator()<false,false,TPaced>();
                }
            };
            if (config_.producer_interval_ns) dispatch.template operator()<true>();
            else dispatch.template operator()<false>();
        }
        const auto active_end = std::chrono::steady_clock::now();
        result.producer_seconds += std::chrono::duration<double>(active_end-active_start).count();
        ++result.completed_rounds;
        if (round_end == config_.messages) channel.finish();
        else if (publication_plan.finish_blocks) channel.finish_block();
        if (config_.round_storage_bytes) {
            // 普通模式等待自然发布；完成块模式在计时外交接尾部，不添加虚假消息。
            // 最后一轮统一 finish。原生 SPSC 每条发布，无需轮末额外操作。
            while (sink.completed_rounds.load(std::memory_order_acquire) < result.completed_rounds)
                std::this_thread::yield();
            result.drain_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now()-active_end).count();
            channel.check();
        }
        round_begin = round_end;
        }
        const auto producer_finish = std::chrono::steady_clock::now();
        while (!sink.done.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto end = std::max(producer_finish,sink.finish_time);
        const double cpu_end = benchmark::cpu_seconds();
        channel.stop(); channel.check();
#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
        if constexpr (requires { channel.end_pool_depth_stats(); }) {
            result.pool_depth = channel.end_pool_depth_stats();
            result.pool_depth_stats_available = true;
        }
#endif
        const auto pressure_end = channel.pressure_stats();
        result.library_pressure_stats_available = pressure_end.available;
        result.writer_pressure_events = pressure_end.writer.pressure_events-pressure_start.writer.pressure_events;
        result.writer_rejected_attempts = pressure_end.writer.dropped_records-pressure_start.writer.dropped_records;
        result.writer_blocked_events = pressure_end.writer.blocked_events-pressure_start.writer.blocked_events;
        result.writer_blocked_ticks = pressure_end.writer.blocked_ticks-pressure_start.writer.blocked_ticks;
        result.copy_memory_full_events = pressure_end.copy.memory_full_events-pressure_start.copy.memory_full_events;
        result.copy_backpressured_frames = pressure_end.copy.memory_backpressured_frames-pressure_start.copy.memory_backpressured_frames;
        if (!sink.valid || sink.received != warmup+config_.messages) throw std::runtime_error("payload/FIFO/count validation failed");
        result.wall_seconds = std::chrono::duration<double>(end-start).count();
        result.seconds = config_.round_storage_bytes ? result.producer_seconds : result.wall_seconds;
        result.m_messages_per_second = config_.messages/result.seconds/1e6;
        result.payload_gb_per_second = config_.messages/result.seconds*TBytes/1e9;
        result.cpu_cores = (cpu_end-cpu_start)/result.wall_seconds;
        result.peak_rss_mib = benchmark::peak_rss_mib();
        result.consumed = config_.messages; result.callback_work_checksum = sink.work_checksum;
        result.callback_processing_seconds = sink.callback_processing_seconds;
        result.timed_callback_messages = sink.timed_callback_messages;
        result.callback_to_producer_ratio = result.callback_processing_seconds/result.producer_seconds;
        result.latency_samples = success_samples.size(); result.failed_latency_samples = failure_samples.size();
        const auto latency_quantiles = quantiles(std::move(success_samples));
        if (config_.mode == MeasureMode::write_latency) result.batch_average_write_ns = latency_quantiles;
        else result.successful_write_ns = latency_quantiles;
        result.failed_write_ns = quantiles(std::move(failure_samples));
        result.resolution_limited = result.latency_samples &&
            (latency_quantiles.p50 < 5*result.latency_quantum_ns || latency_quantiles.p50 < 2*result.timer_overhead_ns.p50 /
                (config_.mode == MeasureMode::write_latency ? config_.latency_batch_size : 1));
        if (result.call_batch_attempts) result.call_batch_mean_ns = batch_elapsed/result.call_batch_attempts;
        return result;
    }
    PerformanceConfig config_;
};
} // namespace TwoBufQueue::performance
