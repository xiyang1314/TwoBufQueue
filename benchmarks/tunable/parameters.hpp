#pragma once
#include "performance_test.hpp"
#include <charconv>
#include <iomanip>
#include <ostream>
#include <string_view>
#include <type_traits>

namespace TwoBufQueue::performance {
// One field table drives the CLI, defaults and result JSON so new parameters remain reproducible.
template<class TConfig, class TFunction> void for_each_parameter(TConfig& c, TFunction function) {
#define BRQ_PARAMETER(label, member) function(label,c.member)
    BRQ_PARAMETER("transport",transport); BRQ_PARAMETER("mode",mode); BRQ_PARAMETER("payload_bytes",payload_bytes);
    BRQ_PARAMETER("messages",messages); BRQ_PARAMETER("warmup_messages",warmup_messages);
    BRQ_PARAMETER("callback_iterations",callback_iterations); BRQ_PARAMETER("verify_payload",verify_payload);
    BRQ_PARAMETER("prepared_input",prepared_input); BRQ_PARAMETER("variable_payload",variable_payload);
    BRQ_PARAMETER("prepared_input_limit_bytes",prepared_input_limit_bytes);
    BRQ_PARAMETER("measure_callback_time",measure_callback_time);
    BRQ_PARAMETER("producer_interval_ns",producer_interval_ns);
    BRQ_PARAMETER("source_block_size",source.block_size); BRQ_PARAMETER("source_pool_min",source.pool_min);
    BRQ_PARAMETER("source_pool_max",source.pool_max); BRQ_PARAMETER("source_queue_capacity",source.queue_capacity);
    BRQ_PARAMETER("writer_backpressure",source.backpressure.mode); BRQ_PARAMETER("writer_spin_count",source.backpressure.spin_count);
    BRQ_PARAMETER("writer_wait_timeout_ms",source.backpressure.wait_get_timeout);
    BRQ_PARAMETER("output_block_size",output.output_block_size); BRQ_PARAMETER("output_frame_capacity",output.output_frame_capacity);
    BRQ_PARAMETER("output_pool_min",output.output_pool_min); BRQ_PARAMETER("output_pool_max",output.output_pool_max);
    BRQ_PARAMETER("output_queue_capacity",output.output_queue_capacity); BRQ_PARAMETER("cancel_claim_mode",output.cancel_claim_mode);
    BRQ_PARAMETER("output_mode",output.output_mode); BRQ_PARAMETER("output_reader_mode",output.output_reader_mode);
    BRQ_PARAMETER("output_reader_count",output.output_reader_count);
    BRQ_PARAMETER("output_retry_wait_us",output.output_retry_wait); BRQ_PARAMETER("output_enable_stats",output.output_enable_stats);
    BRQ_PARAMETER("is_scan_current",output.read_config.is_scan_current);
    BRQ_PARAMETER("reader_spin_count",output.read_config.idle_spin_count);
    BRQ_PARAMETER("rescan_interval_ns",output.read_config.idle_rescan_interval);
    BRQ_PARAMETER("rescan_max_interval_ns",output.read_config.idle_rescan_max_interval);
    BRQ_PARAMETER("consumer_cpu",output.read_config.worker_cpu); BRQ_PARAMETER("producer_cpu",producer_cpu);
    BRQ_PARAMETER("producer_qos",producer_qos); BRQ_PARAMETER("round_storage_bytes",round_storage_bytes);
    BRQ_PARAMETER("fixed_round_boundaries",fixed_round_boundaries);
    BRQ_PARAMETER("spsc_buffer_bytes",spsc_buffer_bytes); BRQ_PARAMETER("prefault_producer_buffers",prefault_producer_buffers);
    BRQ_PARAMETER("finish_timeout_ms",output.read_config.finish_timeout);
    BRQ_PARAMETER("work_wait_min_ms",output.read_config.work_wait_min_interval);
    BRQ_PARAMETER("finish_retry_ms",output.read_config.finish_retry_interval);
    BRQ_PARAMETER("publish_count",publish_count);
    BRQ_PARAMETER("use_write_session",use_write_session);
    BRQ_PARAMETER("rigtorp_write_prefetch",rigtorp_write_prefetch);
    BRQ_PARAMETER("readerwriterqueue_copy_after_zero",readerwriterqueue_copy_after_zero);
    BRQ_PARAMETER("spsc_capacity",spsc_capacity); BRQ_PARAMETER("retry_yield_every",retry_yield_every);
    BRQ_PARAMETER("spsc_idle_spins",spsc_idle_spins); BRQ_PARAMETER("spsc_idle_sleep_ns",spsc_idle_sleep_ns);
    BRQ_PARAMETER("timer",timer); BRQ_PARAMETER("sample_every",sample_every); BRQ_PARAMETER("max_samples",max_samples);
    BRQ_PARAMETER("latency_batch_size",latency_batch_size);
    BRQ_PARAMETER("timer_calibration_samples",timer_calibration_samples); BRQ_PARAMETER("call_batch_size",call_batch_size);
    BRQ_PARAMETER("random_seed",random_seed); BRQ_PARAMETER("timeout_seconds",timeout_seconds);
    BRQ_PARAMETER("buffer_budget_bytes",buffer_budget_bytes);
#undef BRQ_PARAMETER
}
template<class TValue> void assign_parameter(TValue& value, std::string_view text) {
    const auto bad = [] { throw std::invalid_argument("invalid parameter value"); };
    if constexpr (std::is_same_v<TValue,Transport>) {
        if (text == "two_buf_queue") value = Transport::two_buf_queue;
        else if (text == "rigtorp") value = Transport::rigtorp;
        else if (text == "readerwriterqueue") value = Transport::readerwriterqueue;
        else if (text == "atomic_queue") value = Transport::atomic_queue; else bad();
    } else if constexpr (std::is_same_v<TValue,MeasureMode>) {
        if (text == "throughput") value = MeasureMode::throughput;
        else if (text == "write_latency") value = MeasureMode::write_latency;
        else if (text == "single_write_latency") value = MeasureMode::single_write_latency;
        else if (text == "call_batch") value = MeasureMode::call_batch; else bad();
    } else if constexpr (std::is_same_v<TValue,ProducerQos>) {
        if (text == "unchanged") value = ProducerQos::unchanged;
        else if (text == "user_interactive") value = ProducerQos::user_interactive; else bad();
    } else if constexpr (std::is_same_v<TValue,TimerKind>) {
        if (text == "auto") value = TimerKind::automatic;
        else if (text == "steady") value = TimerKind::steady; else bad();
    } else if constexpr (std::is_same_v<TValue,BackpressureMode>) {
        if (text == "drop") value = BackpressureMode::Drop;
        else if (text == "block") value = BackpressureMode::Block; else bad();
    } else if constexpr (std::is_same_v<TValue,CancelClaimMode>) {
        if (text == "tail") value = CancelClaimMode::QueueTail;
        else if (text == "keep") value = CancelClaimMode::Keep; else bad();
    } else if constexpr (std::is_same_v<TValue,HandoffMode>) {
        if (text == "sync") value = HandoffMode::Sync;
        else if (text == "spsc") value = HandoffMode::SPSC;
        else if (text == "mpsc") value = HandoffMode::MPSC;
        else if (text == "spmc") value = HandoffMode::SPMC;
        else if (text == "mpmc") value = HandoffMode::MPMC; else bad();
    } else if constexpr (std::is_same_v<TValue,ReaderMode>) {
        if (text == "polling") value = ReaderMode::Polling;
        else if (text == "wait_signal") value = ReaderMode::WaitSignal; else bad();
    } else if constexpr (std::is_same_v<TValue,bool>) {
        if (text == "true" || text == "1") value = true;
        else if (text == "false" || text == "0") value = false; else bad();
    } else if constexpr (requires { value.count(); }) {
        typename TValue::rep parsed{}; assign_parameter(parsed,text); value = TValue{parsed};
    } else {
        TValue parsed{}; const auto result = std::from_chars(text.data(),text.data()+text.size(),parsed);
        if (result.ec != std::errc{} || result.ptr != text.data()+text.size()) bad();
        value = parsed;
    }
}
inline void set_parameter(PerformanceConfig& config, std::string_view key, std::string_view value) {
    bool found = false;
    for_each_parameter(config,[&](const char* name, auto& field) {
        if (key == name) { assign_parameter(field,value); found = true; }
    });
    if (!found) throw std::invalid_argument("unknown parameter: "+std::string(key));
}
template<class TValue> void write_value(std::ostream& out, const TValue& value) {
    if constexpr (std::is_same_v<TValue,Transport> || std::is_same_v<TValue,MeasureMode>) out << '"' << name(value) << '"';
    else if constexpr (std::is_same_v<TValue,TimerKind>) out << (value == TimerKind::automatic ? "\"auto\"" : "\"steady\"");
    else if constexpr (std::is_same_v<TValue,ProducerQos>) out << (value == ProducerQos::unchanged ? "\"unchanged\"" : "\"user_interactive\"");
    else if constexpr (std::is_same_v<TValue,BackpressureMode>) out << (value == BackpressureMode::Drop ? "\"drop\"" : "\"block\"");
    else if constexpr (std::is_same_v<TValue,CancelClaimMode>) out << (value == CancelClaimMode::QueueTail ? "\"tail\"" : "\"keep\"");
    else if constexpr (std::is_same_v<TValue,HandoffMode>) {
        switch (value) {
        case HandoffMode::Sync: out << "\"sync\""; break;
        case HandoffMode::SPSC: out << "\"spsc\""; break;
        case HandoffMode::MPSC: out << "\"mpsc\""; break;
        case HandoffMode::SPMC: out << "\"spmc\""; break;
        case HandoffMode::MPMC: out << "\"mpmc\""; break;
        }
    }
    else if constexpr (std::is_same_v<TValue,ReaderMode>) out << (value == ReaderMode::Polling ? "\"polling\"" : "\"wait_signal\"");
    else if constexpr (std::is_same_v<TValue,bool>) out << (value ? "true" : "false");
    else if constexpr (requires { value.count(); }) out << value.count();
    else out << value;
}
inline void write_config(std::ostream& out, const PerformanceConfig& config) {
    out << '{'; bool first = true;
    for_each_parameter(config,[&](const char* name, const auto& field) {
        if (!first) out << ','; first = false;
        out << '"' << name << "\":"; write_value(out,field);
    });
    out << '}';
}
inline void write_quantiles(std::ostream& out, Quantiles value) {
    out << "{\"p50\":" << value.p50 << ",\"p99\":" << value.p99 << ",\"p999\":" << value.p999 << ",\"max\":" << value.maximum << '}';
}
#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
inline void write_pool_depth(std::ostream& out, const PoolDepthStats& stats, std::size_t block_bytes) {
    out << "{\"samples\":" << stats.samples
        << ",\"initial_in_use\":" << stats.initial_in_use
        << ",\"initial_allocated\":" << stats.initial_resources
        << ",\"max_in_use\":" << stats.max_in_use
        << ",\"max_allocated\":" << stats.max_resources
        << ",\"in_use_sum\":" << stats.in_use_sum
        << ",\"allocated_sum\":" << stats.resource_sum
        << ",\"free_sum\":" << stats.free_sum
        << ",\"max_in_use_bytes\":" << stats.max_in_use * block_bytes
        << ",\"max_allocated_bytes\":" << stats.max_resources * block_bytes
        << ",\"average_in_use\":";
    if (stats.samples) out << static_cast<double>(stats.in_use_sum)/stats.samples; else out << "null";
    out << ",\"average_allocated\":";
    if (stats.samples) out << static_cast<double>(stats.resource_sum)/stats.samples; else out << "null";
    out << ",\"average_free\":";
    if (stats.samples) out << static_cast<double>(stats.free_sum)/stats.samples; else out << "null";
    out << '}';
}
#endif
inline void write_result(std::ostream& out, const PerformanceResult& r) {
    out << std::setprecision(12) << "{\"config\":"; write_config(out,r.config);
    out << ",\"seconds\":" << r.seconds << ",\"m_messages_per_second\":" << r.m_messages_per_second
        << ",\"timing_scope\":\"" << (r.config.round_storage_bytes ? "producer_rounds" : "end_to_end") << '"'
        << ",\"producer_seconds\":" << r.producer_seconds << ",\"wall_seconds\":" << r.wall_seconds
        << ",\"producer_workload\":\"" << (r.config.prepared_input ? "prepared_input_write_loop" : "generate_and_write_loop") << '"'
        << ",\"input_preparation_seconds\":" << r.input_preparation_seconds
        << ",\"prepared_input_bytes\":" << r.prepared_input_bytes
        << ",\"callback_processing_seconds\":" << r.callback_processing_seconds
        << ",\"callback_to_producer_ratio\":" << r.callback_to_producer_ratio
        << ",\"timed_callback_messages\":" << r.timed_callback_messages
        << ",\"callback_timing_scope\":\"" << (r.config.measure_callback_time ? "round_first_to_last_callback_including_dispatch" : "disabled") << '"'
        << ",\"drain_seconds\":" << r.drain_seconds << ",\"completed_rounds\":" << r.completed_rounds
        << ",\"effective_warmup_messages\":" << r.effective_warmup_messages
        << ",\"publication_alignment\":" << r.publication_alignment
        << ",\"source_record_bytes\":" << r.source_record_bytes
        << ",\"round_message_minimum\":" << r.round_message_minimum
        << ",\"round_message_limit\":" << r.round_message_limit
        << ",\"producer_hard_affinity\":" << (r.producer_hard_affinity ? "true" : "false")
        << ",\"producer_qos_applied\":" << (r.producer_qos_applied ? "true" : "false")
        << ",\"producer_qos_actual\":" << r.producer_qos_actual
        << ",\"payload_gb_per_second\":" << r.payload_gb_per_second << ",\"cpu_cores\":" << r.cpu_cores
        << ",\"peak_rss_mib\":" << r.peak_rss_mib << ",\"consumed\":" << r.consumed
        << ",\"backpressured_messages\":" << r.backpressured_messages
        << ",\"producer_retry_seconds\":" << r.producer_retry_seconds
        << ",\"producer_retry_max_seconds\":" << r.producer_retry_max_seconds
        << ",\"failed_write_calls\":" << r.failed_write_calls << ",\"callback_work_checksum\":" << r.callback_work_checksum
        << ",\"session_block_exhaustions\":" << r.session_block_exhaustions
        << ",\"actual_spsc_capacity\":" << r.actual_spsc_capacity << ",\"nominal_buffer_bytes\":" << r.nominal_buffer_bytes
        << ",\"latency_samples\":" << r.latency_samples << ",\"failed_latency_samples\":" << r.failed_latency_samples
        << ",\"migrated_samples\":" << r.migrated_samples << ",\"timer\":\"" << r.timer_name
        << "\",\"timer_quantum_ns\":" << r.timer_quantum_ns << ",\"timer_overhead_ns\":";
    write_quantiles(out,r.timer_overhead_ns);
    out << ",\"library_backpressure\":";
    if (r.library_pressure_stats_available) {
        out << "{\"scope\":\"after_warmup_through_formal_drain_and_stop\""
            << ",\"writer_pressure_events\":" << r.writer_pressure_events
            << ",\"writer_rejected_attempts\":" << r.writer_rejected_attempts
            << ",\"writer_blocked_events\":" << r.writer_blocked_events
            << ",\"writer_blocked_ticks\":" << r.writer_blocked_ticks
            << ",\"writer_blocked_counter_kind\":\""
#if defined(__i386__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
            << "tsc_ticks"
#else
            << "steady_clock_ticks"
#endif
            << "\",\"copy_memory_full_events\":" << r.copy_memory_full_events
            << ",\"copy_backpressured_frames\":" << r.copy_backpressured_frames << '}';
    } else out << "null";
    out << ",\"successful_write_ns\":";
    if (r.latency_samples && r.config.mode == MeasureMode::single_write_latency) write_quantiles(out,r.successful_write_ns); else out << "null";
    out << ",\"latency_kind\":\"" << (r.config.mode == MeasureMode::write_latency ? "batch_average" :
        r.config.mode == MeasureMode::single_write_latency ? "single_call" : "none") << '"';
    out << ",\"latency_quantum_ns\":" << r.latency_quantum_ns
        << ",\"latency_tail_writes\":" << r.latency_tail_writes << ",\"batch_average_write_ns\":";
    if (r.latency_samples && r.config.mode == MeasureMode::write_latency) write_quantiles(out,r.batch_average_write_ns); else out << "null";
    out << ",\"failed_write_ns\":";
    if (r.failed_latency_samples) write_quantiles(out,r.failed_write_ns); else out << "null";
    out << ",\"resolution_limited\":" << (r.resolution_limited ? "true" : "false")
        << ",\"p999_sample_count_warning\":" << (r.latency_samples && r.latency_samples < 100000 ? "true" : "false")
        << ",\"call_batch_mean_ns\":" << r.call_batch_mean_ns
        << ",\"call_batch_empty_mean_ns\":" << r.call_batch_empty_mean_ns
        << ",\"call_batch_attempts\":" << r.call_batch_attempts;
#if defined(BRQ_ENABLE_POOL_DEPTH_STATS)
    out << ",\"pool_depth\":";
    if (r.pool_depth_stats_available) {
        out << "{\"scope\":\"after_warmup_through_formal_drain_and_stop\""
            << ",\"sample_point\":\"successful_get_after_increment_under_existing_pool_lock\""
            << ",\"average_weighting\":\"successful_acquisitions\",\"writer\":";
        write_pool_depth(out,r.pool_depth.writer,r.config.source.block_size);
        out << ",\"reader\":";
        write_pool_depth(out,r.pool_depth.reader,r.config.output.output_block_size);
        out << '}';
    } else out << "null";
#endif
    out << '}';
}
} // namespace TwoBufQueue::performance
