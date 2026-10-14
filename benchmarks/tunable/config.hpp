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

// Keep the workload fixed within a comparison; search only the other tuning parameters. Allocate all capacities before construction.
struct PerformanceConfig {
    Transport transport = Transport::two_buf_queue;
    MeasureMode mode = MeasureMode::throughput;
    std::size_t payload_bytes = 8;                 // Supports only 8 / 16 / 32 / 64 B; no timing metadata is added.
    std::uint64_t messages = 2'000'000;            // Successful production/consumption count, identical for every library.
    std::uint64_t warmup_messages = 50'000;
    std::uint32_t callback_iterations = 0;        // Simulated business iterations per message; zero performs no simulated computation.
    bool measure_callback_time = false;           // Time the first-to-last callback interval per round, including dispatch and waiting within the round.
    bool verify_payload = false;                  // true verifies every payload byte; always verify sequence numbers and counts.
    bool prepared_input = true;                   // Prepare the full round payload before timing writes; exclude message generation.
    bool variable_payload = true;                 // The first word is the sequence; every other word also changes with that sequence.
    std::size_t prepared_input_limit_bytes = 256 * 1024 * 1024; // Separate input-storage limit, excluded from queue capacity.
    std::uint64_t producer_interval_ns = 0;       // Zero sends continuously; nonzero fixes the offered workload and is not tuned.

    WriterSourceConfig source{.pool_min = 8, .pool_max = 8};                  // Block size, pool min/max, handoff capacity and Writer backpressure.
    TwoBufQueueOptions output{.output_pool_min = 2, .output_pool_max = 2};                  // Layer-2 copying, output pool/queue and ReadConfig.
    std::uint32_t publish_count = 1;              // Powers of two from 1..1024, or 10; UINT32_MAX publishes only on cold handoff paths.
    bool readerwriterqueue_copy_after_zero = false; // Zero the queue slot before full memcpy; affects ReaderWriterQueue only.
    bool rigtorp_write_prefetch = true;           // Hint destination write-prefetch before constructing the message; affects rigtorp only.
    bool use_write_session = false;               // Single-block Session nested loop; retain ordinary write for comparison.
    std::size_t spsc_capacity = 0;                // Zero derives capacity from source-pool record capacity; native queues may be tuned independently.
    std::int32_t producer_cpu = -1;
    ProducerQos producer_qos = ProducerQos::unchanged; // macOS scheduling preference, not hard CPU affinity.
    std::size_t round_storage_bytes = 0;           // Zero uses the original end-to-end mode; otherwise derive a payload limit and stop at a natural publication boundary.
    bool fixed_round_boundaries = false;          // Stop exactly at the round byte limit and hand off tails outside timing; all implementations share boundaries.
    std::size_t spsc_buffer_bytes = 0;             // When capacity is zero, optionally derive native queue capacity from this data-byte budget.
    bool prefault_producer_buffers = false;       // Touch producer data pages before formal timing.
    std::uint32_t retry_yield_every = 1024;        // Failed-retry pause count before yield; zero always pauses.
    std::uint32_t spsc_idle_spins = 1000;
    std::uint64_t spsc_idle_sleep_ns = 0;         // After spinning, zero yields and nonzero sleeps; report CPU cost separately.

    TimerKind timer = TimerKind::automatic;       // x86 invariant TSC + RDTSCP; native ticks on macOS; steady_clock elsewhere.
    std::uint32_t latency_batch_size = 1000;      // Successful writes per write_latency group; record group duration divided by its count.
    std::uint32_t sample_every = 32;              // single_write_latency only: sample at randomized intervals.
    std::size_t max_samples = 1'000'000;           // Reserve sample capacity; never grow it inside a timed region.
    std::uint32_t timer_calibration_samples = 16384;
    std::uint32_t call_batch_size = 256;          // Supplementary batch mean; never interpret it as a single-call quantile.
    std::uint64_t random_seed = 42;
    std::uint32_t timeout_seconds = 60;           // Terminate this independent test process on timeout.
    std::size_t buffer_budget_bytes = 64 * 1024 * 1024; // Limit nominal data-buffer storage, not the process's total RSS.

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
