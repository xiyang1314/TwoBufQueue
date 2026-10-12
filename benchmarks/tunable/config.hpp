#pragma once
#include <TwoBufQueue.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace TwoBufQueue::performance {
enum class Transport { two_buf_queue, rigtorp, readerwriterqueue, atomic_queue };
enum class MeasureMode { throughput, write_latency, single_write_latency, call_batch };
enum class TimerKind { automatic, steady };
enum class ProducerQos { unchanged, user_interactive };

// 同一轮对比固定 workload；搜索只改其余调优项。所有容量均为构造前分配。
struct PerformanceConfig {
    Transport transport = Transport::two_buf_queue;
    MeasureMode mode = MeasureMode::throughput;
    std::size_t payload_bytes = 8;                 // 仅支持 8 / 16 / 32 / 64；不加计时元数据
    std::uint64_t messages = 2'000'000;            // 正式成功写入/消费次数，各库完全相同
    std::uint64_t warmup_messages = 50'000;
    std::uint32_t callback_iterations = 0;        // 每条消息的模拟业务循环；0 不做模拟计算
    bool measure_callback_time = false;           // 每轮首/末回调计时，包含轮内消息分发/等待
    bool verify_payload = false;                  // true 检查全部字节；始终检查序号和条数
    bool prepared_input = true;                   // 提前准备本轮完整负载，写入计时不含消息生成
    bool variable_payload = true;                 // 首字为序号，其余每个字也随序号变化
    std::size_t prepared_input_limit_bytes = 256 * 1024 * 1024; // 独立输入存储上限，不算队列容量
    std::uint64_t producer_interval_ns = 0;       // 0 连续写；非 0 是固定工作负载，不参与调优

    WriterSourceConfig source{.pool_min = 8, .pool_max = 8};                  // 块大小、池 min/max、交接容量、Writer 背压
    TwoBufQueueOptions output{.output_pool_min = 2, .output_pool_max = 2};                  // 第二层复制、输出池/队列、ReadConfig
    std::uint32_t publish_count = 1;              // 1..1024 的 2 次幂或 10；UINT32_MAX 仅在交接等冷路径发布
    bool readerwriterqueue_copy_after_zero = false; // 队列槽位先清零再完整 memcpy；仅影响 ReaderWriterQueue
    bool rigtorp_write_prefetch = true;           // 构造消息前提示目标写预取；只影响 rigtorp
    bool use_write_session = false;               // 单块 Session 双重循环；保留普通 write 对照
    std::size_t spsc_capacity = 0;                // 0 按源池可容纳记录数计算；第三方可单独搜索
    std::int32_t producer_cpu = -1;
    ProducerQos producer_qos = ProducerQos::unchanged; // macOS 调度偏好，不能当作硬绑核
    std::size_t round_storage_bytes = 0;           // 0=原端到端模式；非0按 payload 计算上限，再停在自然发布边界
    bool fixed_round_boundaries = false;          // 分轮时精确按字节上限停写，计时外交接尾块；所有实现共用边界
    std::size_t spsc_buffer_bytes = 0;             // capacity=0 时可按数据字节指定第三方容量
    bool prefault_producer_buffers = false;       // 正式计时前触碰写端数据页
    std::uint32_t retry_yield_every = 1024;        // 失败重试时多少次 pause 后 yield；0 始终 pause
    std::uint32_t spsc_idle_spins = 1000;
    std::uint64_t spsc_idle_sleep_ns = 0;         // 自旋后 0=yield，非 0=sleep；CPU 成本单独报告

    TimerKind timer = TimerKind::automatic;       // x86 invariant TSC+RDTSCP；Mac 原生 ticks；其他 steady
    std::uint32_t latency_batch_size = 1000;      // write_latency 每组成功写入次数，记录组耗时 / 次数
    std::uint32_t sample_every = 32;              // 仅 single_write_latency：随机间隔采样
    std::size_t max_samples = 1'000'000;           // 预留样本容量；计时区不扩容
    std::uint32_t timer_calibration_samples = 16384;
    std::uint32_t call_batch_size = 256;          // 批次均值补充，绝不当作单次调用分位数
    std::uint64_t random_seed = 42;
    std::uint32_t timeout_seconds = 60;           // 超时终止该独立测试进程
    std::size_t buffer_budget_bytes = 64 * 1024 * 1024; // 限制名义数据缓冲，不等于整个进程 RSS

    void validate() const {
        const auto bad = [](const char* text) { throw std::invalid_argument(text); };
        if (transport != Transport::two_buf_queue && transport != Transport::rigtorp &&
            transport != Transport::readerwriterqueue && transport != Transport::atomic_queue)
            bad("invalid transport");
        if (mode != MeasureMode::throughput && mode != MeasureMode::write_latency &&
            mode != MeasureMode::single_write_latency && mode != MeasureMode::call_batch)
            bad("invalid measurement mode");
        if (timer != TimerKind::automatic && timer != TimerKind::steady) bad("invalid timer");
        if (producer_qos != ProducerQos::unchanged && producer_qos != ProducerQos::user_interactive)
            bad("invalid producer QoS");
        if (payload_bytes != 8 && payload_bytes != 16 && payload_bytes != 32 && payload_bytes != 64)
            bad("payload_bytes must be 8, 16, 32 or 64");
        if (!messages || messages > (std::uint64_t{1} << 60) || warmup_messages > (std::uint64_t{1} << 60))
            bad("invalid message count");
        if (!sample_every || !max_samples || timer_calibration_samples < 100 || !latency_batch_size || !call_batch_size || !timeout_seconds)
            bad("sample/timer/batch/timeout values must be positive (timer samples >= 100)");
        if (mode == MeasureMode::write_latency && messages < latency_batch_size)
            bad("write_latency requires at least latency_batch_size messages");
        if (prepared_input && (!prepared_input_limit_bytes ||
            std::min(messages,round_messages()) > prepared_input_limit_bytes / payload_bytes))
            bad("prepared round input exceeds prepared_input_limit_bytes; use rounds or increase the input limit");
        if (publish_count != 10 && publish_count != (std::numeric_limits<std::uint32_t>::max)() &&
            (!publish_count || publish_count > 1024 || (publish_count & (publish_count-1))))
            bad("publish_count must be 10, UINT32_MAX or a power of two from 1 through 1024");
        if (use_write_session && (transport != Transport::two_buf_queue ||
            publish_count != (std::numeric_limits<std::uint32_t>::max)() ||
            (mode != MeasureMode::throughput && mode != MeasureMode::write_latency) ||
            !prepared_input || producer_interval_ns || output.output_mode == HandoffMode::Sync))
            bad("use_write_session requires TwoBufQueue/MAX asynchronous continuous prepared throughput/write_latency");
        if (round_storage_bytes) {
            if (round_storage_bytes < payload_bytes || producer_interval_ns)
                bad("rounds require a positive record capacity and continuous input");
            if (mode == MeasureMode::call_batch) bad("call_batch reference does not support rounds");
            if (mode == MeasureMode::write_latency && round_messages() < latency_batch_size)
                bad("a round must fit a full latency group");
            if (transport == Transport::two_buf_queue && output.output_mode == HandoffMode::Sync)
                bad("producer rounds require asynchronous copy and callbacks");
        }
        if (measure_callback_time && !round_storage_bytes)
            bad("callback processing time requires producer rounds");
        if (source.block_size < payload_bytes || !source.pool_min || source.pool_max < source.pool_min ||
            source.queue_capacity < 2 || source.thread_id != 0) bad("invalid source configuration");
        if (output.output_block_size < source.block_size || !output.output_frame_capacity ||
            !output.output_pool_min || output.output_pool_max < output.output_pool_min ||
            !output.output_queue_capacity || output.read_layer1_thread_count != 1)
            bad("output block must fit a source block; this benchmark uses one writer and one reader");
        const auto& r = output.read_config;
        if (r.idle_rescan_interval.count() <= 0 || r.idle_rescan_max_interval < r.idle_rescan_interval ||
            r.work_wait_min_interval.count() <= 0 || r.finish_retry_interval.count() <= 0 || r.finish_timeout.count() <= 0)
            bad("invalid reader intervals");
        const auto max_wait = std::chrono::duration_cast<std::chrono::milliseconds>(AutoResetEvent::Clock::duration::max());
        if (r.finish_timeout > max_wait || r.work_wait_min_interval > max_wait || r.finish_retry_interval > max_wait ||
            source.backpressure.wait_get_timeout.count() <= 0 || source.backpressure.wait_get_timeout > max_wait)
            bad("wait duration exceeds clock range");
        if (producer_interval_ns > static_cast<std::uint64_t>((std::chrono::nanoseconds::max)().count()) / messages ||
            spsc_idle_sleep_ns > static_cast<std::uint64_t>((std::chrono::nanoseconds::max)().count()))
            bad("interval would overflow the clock");
        if ((mode == MeasureMode::call_batch || mode == MeasureMode::write_latency) && producer_interval_ns)
            bad("grouped measurement requires continuous input; use single_write_latency for paced calls");
        if (producer_cpu < -1 || r.worker_cpu < -1) bad("CPU IDs must be -1 or nonnegative");
        if (source.backpressure.mode != BackpressureMode::Drop && source.backpressure.mode != BackpressureMode::Block)
            bad("invalid writer backpressure mode");
        if (output.cancel_claim_mode != CancelClaimMode::QueueTail && output.cancel_claim_mode != CancelClaimMode::Keep)
            bad("invalid output cancellation mode");
        if (transport == Transport::two_buf_queue) {
            HandoffOptions handoff;
            handoff.mode = output.output_mode;
            handoff.reader_mode = output.output_reader_mode;
            handoff.reader_count = output.output_reader_count;
            handoff.retry_wait = output.output_retry_wait;
            (void)validate_handoff_options(handoff);
            if (output.output_reader_count != 1)
                bad("performance sink requires one output callback thread");
        }
        if (!buffer_budget_bytes) bad("buffer_budget_bytes must be positive");
        if (source.block_size > buffer_budget_bytes / source.pool_max ||
            output.output_block_size > buffer_budget_bytes / output.output_pool_max)
            bad("buffer budget exceeded");
        if (source.block_size * source.pool_max > buffer_budget_bytes - output.output_block_size * output.output_pool_max)
            bad("source plus output buffer budget exceeded");
    }
    [[nodiscard]] std::size_t requested_capacity() const noexcept {
        return spsc_capacity ? spsc_capacity : spsc_buffer_bytes ? spsc_buffer_bytes/payload_bytes
            : source.pool_max * (source.block_size / (payload_bytes));
    }
    [[nodiscard]] std::uint64_t round_messages() const noexcept {
        return round_storage_bytes ? round_storage_bytes/(payload_bytes) : messages;
    }
};
inline const char* name(Transport value) {
    switch (value) {
    case Transport::two_buf_queue: return "two_buf_queue";
    case Transport::rigtorp: return "rigtorp";
    case Transport::readerwriterqueue: return "readerwriterqueue";
    case Transport::atomic_queue: return "atomic_queue";
    }
    return "unknown";
}
inline const char* name(MeasureMode value) {
    switch (value) {
    case MeasureMode::throughput: return "throughput";
    case MeasureMode::write_latency: return "write_latency";
    case MeasureMode::single_write_latency: return "single_write_latency";
    case MeasureMode::call_batch: return "call_batch";
    }
    return "unknown";
}
} // namespace TwoBufQueue::performance
