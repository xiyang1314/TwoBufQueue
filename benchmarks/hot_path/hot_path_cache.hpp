#pragma once
#include "../tunable/parameters.hpp"
#include <bit>
#include <random>
#include <span>

namespace TwoBufQueue::performance::hot_path {
inline constexpr std::size_t MiB = 1024 * 1024;
enum class Scenario { paused, slow, fast };
inline const char* scenario_name(Scenario s) {
    switch (s) { case Scenario::paused: return "paused"; case Scenario::slow: return "slow"; case Scenario::fast: return "fast"; }
    return "unknown";
}
struct Config {
    PerformanceConfig queue;
    bool baseline = false;
    bool sampled = false;
    bool grouped = false;
    bool arithmetic = false;
    bool full_oracle = false;
    Scenario scenario = Scenario::paused;
    std::size_t business_bytes = 96 * 1024;
    std::uint32_t business_accesses = 32;
    std::uint32_t business_warmup_steps = 20'000;
    std::uint32_t preparation_rounds = 2;
    Config() {
        queue.payload_bytes = 32;
        queue.messages = 10'000'000;
        queue.warmup_messages = 0;
        queue.callback_iterations = 0;
        queue.prepared_input = false;
        queue.verify_payload = true;
        queue.source.block_size = 16 * 1024;
        queue.source.pool_min = queue.source.pool_max = 4;
        queue.source.queue_capacity = 16;
        queue.source.backpressure.mode = BackpressureMode::Drop;
        queue.output.output_block_size = queue.source.block_size;
        queue.output.output_pool_min = queue.output.output_pool_max = 256 * MiB / queue.output.output_block_size;
        queue.output.output_frame_capacity = 4;
        queue.output.output_queue_capacity = queue.output.output_pool_max + 2;
        queue.output.output_mode = HandoffMode::SPSC;
        queue.output.read_config.is_scan_current = false;
        queue.output.read_config.idle_rescan_interval = std::chrono::milliseconds{1};
        queue.output.read_config.idle_rescan_max_interval = std::chrono::milliseconds{1};
        queue.output.read_config.finish_timeout = std::chrono::seconds{120};
        queue.publish_count = UINT32_MAX;
        queue.spsc_buffer_bytes = 256 * MiB;
        queue.round_storage_bytes = (256 * MiB / 5) * 4;
        queue.fixed_round_boundaries = true;
        queue.prefault_producer_buffers = true;
        queue.sample_every = 256;
        queue.max_samples = 100'000;
        queue.timeout_seconds = 180;
        queue.buffer_budget_bytes = 512 * MiB;
#if defined(__APPLE__)
        queue.producer_qos = ProducerQos::user_interactive;
#endif
    }
    void validate() const {
        queue.validate();
        if (queue.payload_bytes != 32 || queue.use_write_session || queue.publish_count != UINT32_MAX ||
            queue.output.read_config.is_scan_current || !queue.fixed_round_boundaries ||
            !queue.round_storage_bytes || queue.prepared_input || queue.producer_interval_ns)
            throw std::invalid_argument("hot-path showcase uses 32B ordinary publishMAX/completed blocks, fixed rounds and generated business messages");
        if (business_bytes < 64 || business_bytes % 64 || business_bytes / 64 > UINT32_MAX ||
            !business_accesses || preparation_rounds > 10)
            throw std::invalid_argument("invalid business or preparation parameters");
        const auto output_bytes = queue.output.output_block_size * queue.output.output_pool_max;
        if (queue.round_storage_bytes > (output_bytes / 5) * 4)
            throw std::invalid_argument("round data must not exceed 80 percent of fixed output capacity");
        if (scenario == Scenario::slow && !queue.callback_iterations)
            throw std::invalid_argument("slow callback requires positive callback_iterations");
    }
};

// One out-of-line business implementation shared by all transport instantiations.
// A shuffled linked cycle visits every cache line; no modulo is used in the hot loop.
class Business {
    struct alignas(64) Entry { std::uint32_t next; std::uint32_t pad; std::uint64_t value; std::array<std::byte,48> unused{}; };
    std::vector<Entry> entries_;
    std::uint32_t cursor_ = 0;
    std::uint64_t state_;
    std::uint32_t accesses_;
    bool arithmetic_;
public:
    explicit Business(const Config& c) : entries_(c.arithmetic ? 0 : c.business_bytes / 64),
        state_(c.queue.random_seed + 1), accesses_(c.business_accesses), arithmetic_(c.arithmetic) {
        if (arithmetic_) return;
        std::vector<std::uint32_t> order(entries_.size());
        std::iota(order.begin(),order.end(),0);
        std::mt19937_64 random(c.queue.random_seed);
        std::shuffle(order.begin(),order.end(),random);
        for (std::size_t i = 0; i < order.size(); ++i) {
            entries_[order[i]].next = order[(i+1) % order.size()];
            entries_[order[i]].value = detail::payload_word(order[i],1);
        }
        cursor_ = order.front();
    }
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    std::uint64_t step() noexcept {
        auto value = state_;
        if (arithmetic_) {
            for (std::uint32_t i = 0; i < accesses_; ++i) {
                value ^= value << 13; value ^= value >> 7; value ^= value << 17;
                value += 0x9e3779b97f4a7c15ULL;
            }
        } else {
            auto cursor = cursor_;
            for (std::uint32_t i = 0; i < accesses_; ++i) {
                auto& entry = entries_[cursor];
                value = std::rotl(value ^ entry.value,7) + 0x9e3779b97f4a7c15ULL;
                entry.value = value;
                cursor = entry.next;
            }
            cursor_ = cursor;
        }
        state_ = value;
        return value;
    }
};
inline Payload<32> message(std::uint64_t seq, std::uint64_t business) noexcept {
    Payload<32> p;
    const std::array<std::uint64_t,4> words{seq,business,detail::payload_word(seq,2),detail::payload_word(seq,3)^business};
    std::memcpy(p.bytes.data(),words.data(),32);
    return p;
}
inline std::uint64_t checksum(const Payload<32>& p) noexcept {
    std::array<std::uint64_t,4> words;
    std::memcpy(words.data(),p.bytes.data(),32);
    return words[0] + std::rotl(words[1],11) + std::rotl(words[2],23) + std::rotl(words[3],37);
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline std::uint64_t callback_work(std::uint64_t value, std::uint32_t loops) noexcept {
    for (std::uint32_t i=0; i<loops; ++i) {
        value ^= value << 13; value ^= value >> 7; value ^= value << 17;
        value += 0x9e3779b97f4a7c15ULL;
    }
    return value;
}
class Sink {
    // Phase metadata is published before its first queue publication. Only the
    // consumer reads it, once per phase; producer updates after completion acquire.
    std::uint64_t phase_end_ = 0, phase_id_ = 0;
    bool phase_pause_ = false;
    std::uint32_t phase_loops_ = 0;
    std::uint64_t local_end_ = 0, local_phase_ = 0;
    std::uint32_t local_loops_ = 0;
    std::chrono::steady_clock::time_point callback_begin_{};
    std::uint64_t oracle_begin_ = UINT64_MAX;
    const std::vector<Payload<32>>* oracle_ = nullptr;
    std::uint64_t received_ = 0;
public:
    alignas(128) std::atomic<bool> gate{true};
    alignas(128) std::atomic<std::uint64_t> armed{0}, completed{0}, gate_entered{0};
    std::uint64_t received_checksum = 0, work_checksum = 0, oracle_checked = 0;
    double last_callback_seconds = 0;
    bool valid = true;
    void set_oracle(std::uint64_t first, const std::vector<Payload<32>>* oracle) noexcept {
        oracle_begin_ = first; oracle_ = oracle;
    }
    void begin(std::uint64_t end, bool pause, std::uint32_t loops) noexcept {
        phase_end_ = end; phase_pause_ = pause; phase_loops_ = loops;
        gate.store(!pause,std::memory_order_release);
        armed.store(++phase_id_,std::memory_order_release);
    }
    void release() noexcept { gate.store(true,std::memory_order_release); gate.notify_all(); }
    void wait() const noexcept {
        while (completed.load(std::memory_order_acquire) != phase_id_) std::this_thread::yield();
    }
    std::uint64_t received() const noexcept { return received_; } // after phase wait
    void operator()(std::int32_t source, const std::byte* ptr, std::size_t size) noexcept {
        if (received_ == local_end_) {
            local_phase_ = armed.load(std::memory_order_acquire);
            local_end_ = phase_end_; local_loops_ = phase_loops_;
            if (phase_pause_) {
                gate_entered.store(local_phase_,std::memory_order_release);
                while (!gate.load(std::memory_order_acquire)) gate.wait(false,std::memory_order_acquire);
            }
            callback_begin_ = std::chrono::steady_clock::now();
        }
        if (source != 0 || size != 32) valid = false;
        if (size == 32) {
            Payload<32> p;
            std::memcpy(p.bytes.data(),ptr,32);
            std::array<std::uint64_t,4> words;
            std::memcpy(words.data(),ptr,32);
            valid &= words[0] == received_ && words[2] == detail::payload_word(received_,2) &&
                words[3] == (detail::payload_word(received_,3) ^ words[1]);
            received_checksum += checksum(p);
            if (oracle_ && received_ >= oracle_begin_ && received_-oracle_begin_ < oracle_->size()) {
                valid &= p.bytes == (*oracle_)[received_-oracle_begin_].bytes;
                ++oracle_checked;
            }
            if (local_loops_) work_checksum = callback_work(work_checksum ^ words[0],local_loops_);
        }
        if (++received_ == local_end_) {
            last_callback_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-callback_begin_).count();
            completed.store(local_phase_,std::memory_order_release);
        }
    }
};
struct BaselineChannel {
    BaselineChannel(const PerformanceConfig&,Sink&) {}
    bool push(const Payload<32>&) noexcept { return true; }
    void finish_block() noexcept {}
    void finish() noexcept {}
    void stop() noexcept {}
    void check() const {}
    std::size_t capacity() const noexcept { return 0; }
    PressureSnapshot pressure_stats() const noexcept { return {}; }
};
struct Result {
    Config config;
    double producer_seconds = 0, drain_seconds = 0, callback_seconds = 0;
    double m_business_per_second = 0, process_cpu_seconds = 0, peak_rss_mib = 0;
    std::uint64_t backpressured_messages = 0, failed_attempts = 0;
    std::uint64_t formal_checksum = 0, business_checksum = 0, received_checksum = 0, callback_checksum = 0;
    std::uint64_t consumed = 0, oracle_checked = 0, preparation_messages = 0, completed_rounds = 0;
    std::uint64_t source_pressure_events = 0, copy_memory_full_events = 0;
    std::uint64_t minor_faults_timed_process = 0;
    std::uint64_t copied_while_paused_bytes = 0, max_paused_backlog_bytes = 0, pause_confirmed_rounds = 0;
    std::size_t actual_capacity = 0, samples = 0, migrated_samples = 0, group_samples = 0;
    std::uint64_t group_tail_iterations = 0;
    Quantiles group_average_iteration_ns{};
    double body_mean_ns = 0, timer_quantum_ns = 0;
    Quantiles body_ns{}, iteration_ns{}, timer_overhead_ns{};
    std::string timer_name;
    bool qos_applied = false, hard_affinity = false;
};
inline std::uint64_t minor_faults() noexcept {
#if defined(_WIN32)
    return 0;
#else
    rusage r{}; return getrusage(RUSAGE_SELF,&r) ? 0 : static_cast<std::uint64_t>(r.ru_minflt);
#endif
}
template<class TChannel> Result run_with(const Config& c) {
    Result r; r.config = c;
    detail::Watchdog watchdog(c.queue.timeout_seconds);
    Sink sink;
    TChannel channel(c.queue,sink);
    // Unblock a paused callback before the channel is destroyed on any exception.
    struct Release { Sink& sink; ~Release() { sink.release(); } } release{sink};
    detail::ProducerAffinity affinity(c.queue.producer_cpu);
    PerformanceResult schedule;
    detail::ProducerScheduling scheduling(c.queue.producer_qos,schedule);
    r.qos_applied = schedule.producer_qos_applied; r.hard_affinity = c.queue.producer_cpu >= 0;
    r.actual_capacity = channel.capacity();
    const auto limit = c.queue.round_messages();
    if (!c.baseline && r.actual_capacity && r.actual_capacity <= limit)
        throw std::invalid_argument("native capacity must exceed a full controlled-backlog round");
    std::uint64_t sent = 0, expected_checksum = 0;
    const auto retry = [&](const Payload<32>& p) {
        std::uint32_t failures = 0;
        while (!channel.push(p)) {
            if (++failures % std::max(1u,c.queue.retry_yield_every) == 0) std::this_thread::yield();
            else cpu_relax();
        }
    };
    // Explicit gated warm-up touches output pages through the real copy path.
    // No business state or timed sample is generated in these preparation rounds.
    if (!c.baseline) for (std::uint32_t pass=0; pass<c.preparation_rounds; ++pass) {
        sink.begin(sent+limit,true,0);
        for (std::uint64_t i=0; i<limit; ++i) {
            auto p = message(sent++,0); expected_checksum += checksum(p); retry(p);
        }
        channel.finish_block();
        if constexpr (requires { TChannel::message_prefix_size; }) {
            while (channel.pressure_stats().copy.saved_data_bytes < sent*32) { channel.check(); std::this_thread::yield(); }
        }
        sink.release(); sink.wait(); channel.check();
    }
    r.preparation_messages = sent;
    const auto before_pressure = channel.pressure_stats();
    Business business(c);
    for (std::uint32_t i=0; i<c.business_warmup_steps; ++i) (void)business.step();
    std::vector<Payload<32>> oracle;
    if (c.full_oracle) {
        Business reference(c);
        for (std::uint32_t i=0; i<c.business_warmup_steps; ++i) (void)reference.step();
        oracle.reserve(c.queue.messages);
        for (std::uint64_t i=0; i<c.queue.messages; ++i) oracle.push_back(message(sent+i,reference.step()));
        sink.set_oracle(sent,&oracle);
        // Rewarm the business after constructing the validation oracle.
        business = Business(c);
        for (std::uint32_t i=0; i<c.business_warmup_steps; ++i) (void)business.step();
    }
    ProbeClock clock(c.queue);
    r.timer_name = clock.label(); r.timer_quantum_ns = clock.quantum_ns(); r.timer_overhead_ns = clock.overhead();
    std::vector<double> body_samples, iteration_samples, group_samples;
    group_samples.reserve(static_cast<std::size_t>(c.queue.messages/c.queue.latency_batch_size)+1);
    body_samples.reserve(c.queue.max_samples); iteration_samples.reserve(c.queue.max_samples);
    const auto checksum_before = expected_checksum;
    const auto cpu_before = benchmark::cpu_seconds();
    std::uint64_t formal = 0, next_sample = 0, sample_random = c.queue.random_seed + 1;
    const bool pause = c.scenario == Scenario::paused;
    const auto loops = c.scenario == Scenario::slow ? c.queue.callback_iterations : 0;
    while (formal < c.queue.messages) {
        const auto count = std::min<std::uint64_t>(limit,c.queue.messages-formal);
        const auto copy_before_phase = channel.pressure_stats().copy.saved_data_bytes;
        if (!c.baseline) sink.begin(sent+count,pause,loops);
        const auto faults_before = minor_faults();
        const auto begin = clock.read();
        // Throughput and sparse single-iteration timing have separate loops.
        // No per-message clocks or changing atomic phase state in throughput.
        const auto write_one = [&](const Payload<32>& p) {
            if (!channel.push(p)) {
                ++r.backpressured_messages;
                std::uint32_t failed = 0;
                do {
                    ++r.failed_attempts;
                    if (++failed % std::max(1u,c.queue.retry_yield_every) == 0) std::this_thread::yield();
                    else cpu_relax();
                } while (!channel.push(p));
            }
        };
        if (c.grouped) {
            std::uint64_t i = 0;
            while (i < count) {
                const auto batch = std::min<std::uint64_t>(c.queue.latency_batch_size,count-i);
                const auto a = clock.read();
                for (std::uint64_t j=0; j<batch; ++j) {
                    const auto value = business.step(); const auto p = message(sent++,value);
                    write_one(p); r.business_checksum += value; expected_checksum += checksum(p);
                }
                const auto b = clock.read();
                if (batch == c.queue.latency_batch_size) group_samples.push_back(clock.elapsed(a,b)/static_cast<double>(batch));
                else r.group_tail_iterations += batch;
                i += batch;
            }
        } else if (!c.sampled) {
            for (std::uint64_t i=0; i<count; ++i) {
                const auto value = business.step();
                const auto p = message(sent++,value);
                write_one(p);
                r.business_checksum += value; expected_checksum += checksum(p);
            }
        } else {
            for (std::uint64_t i=0; i<count; ++i) {
                const bool sample = formal+i == next_sample && body_samples.size() < c.queue.max_samples;
                if (sample) {
                    sample_random ^= sample_random << 13; sample_random ^= sample_random >> 7; sample_random ^= sample_random << 17;
                    next_sample += std::max(1u,c.queue.sample_every/2) + sample_random % c.queue.sample_every;
                    const auto a = clock.read();
                    const auto value = business.step();
                    const auto b = clock.read();
                    const auto p = message(sent++,value);
                    write_one(p);
                    const auto d = clock.read();
                    r.business_checksum += value; expected_checksum += checksum(p);
                    if (a.cpu == b.cpu && b.cpu == d.cpu && a.ticks <= b.ticks && b.ticks <= d.ticks) {
                        body_samples.push_back(clock.elapsed(a,b)); iteration_samples.push_back(clock.elapsed(a,d));
                    } else ++r.migrated_samples;
                } else {
                    const auto value = business.step(); const auto p = message(sent++,value);
                    write_one(p); r.business_checksum += value; expected_checksum += checksum(p);
                }
            }
        }
        const auto end = clock.read();
        r.producer_seconds += clock.elapsed(begin,end) * 1e-9;
        r.minor_faults_timed_process += minor_faults()-faults_before;
        // Tail publication, callback release and drain are outside hot-loop timing.
        const auto drain_begin = std::chrono::steady_clock::now();
        if (!c.baseline) {
            channel.finish_block();
            if (pause) {
                r.max_paused_backlog_bytes = std::max(r.max_paused_backlog_bytes,count*32);
                r.copied_while_paused_bytes += channel.pressure_stats().copy.saved_data_bytes-copy_before_phase;
                if (sink.gate_entered.load(std::memory_order_acquire) == sink.armed.load(std::memory_order_acquire))
                    ++r.pause_confirmed_rounds;
            }
            sink.release(); sink.wait(); channel.check();
            r.callback_seconds += sink.last_callback_seconds;
        }
        r.drain_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now()-drain_begin).count();
        formal += count; ++r.completed_rounds;
    }
    channel.finish(); channel.stop(); channel.check();
    r.process_cpu_seconds = benchmark::cpu_seconds()-cpu_before;
    r.peak_rss_mib = benchmark::peak_rss_mib();
    r.consumed = c.baseline ? formal : sink.received()-r.preparation_messages;
    r.formal_checksum = expected_checksum-checksum_before;
    r.received_checksum = sink.received_checksum;
    r.callback_checksum = sink.work_checksum;
    r.oracle_checked = sink.oracle_checked;
    if (!c.baseline && (!sink.valid || sink.received() != sent || sink.received_checksum != expected_checksum ||
        (c.full_oracle && sink.oracle_checked != formal))) throw std::runtime_error("FIFO/full payload/checksum verification failed");
    const auto after_pressure = channel.pressure_stats();
    if (after_pressure.available) {
        r.source_pressure_events = after_pressure.writer.pressure_events-before_pressure.writer.pressure_events;
        r.copy_memory_full_events = after_pressure.copy.memory_full_events-before_pressure.copy.memory_full_events;
    }
    r.group_samples = group_samples.size();
    r.group_average_iteration_ns = quantiles(std::move(group_samples));
    r.samples = body_samples.size();
    if (r.samples) r.body_mean_ns = std::accumulate(body_samples.begin(),body_samples.end(),0.0)/r.samples;
    r.body_ns = quantiles(std::move(body_samples)); r.iteration_ns = quantiles(std::move(iteration_samples));
    r.m_business_per_second = static_cast<double>(formal)/r.producer_seconds/1e6;
    return r;
}
inline Result run(Config c) {
    c.validate();
    if (c.baseline) return run_with<BaselineChannel>(c);
    using P = Payload<32>;
    switch (c.queue.transport) {
    case Transport::two_buf_queue: return run_with<TwoBufChannel<UINT32_MAX,Sink,32>>(c);
    case Transport::rigtorp:
        if (c.queue.rigtorp_write_prefetch) return run_with<SpscChannel<Sink,RigtorpAdapter<P,true>>>(c);
        return run_with<SpscChannel<Sink,RigtorpAdapter<P,false>>>(c);
    case Transport::readerwriterqueue:
        if (c.queue.readerwriterqueue_copy_after_zero) return run_with<SpscChannel<Sink,ReaderWriterAdapter<P,true>>>(c);
        return run_with<SpscChannel<Sink,ReaderWriterAdapter<P,false>>>(c);
    case Transport::atomic_queue: return run_with<SpscChannel<Sink,AtomicAdapter<P>>>(c);
    }
    throw std::invalid_argument("transport unavailable");
}
class HotPathCacheTest {
    Config config_;
public:
    explicit HotPathCacheTest(Config config) : config_(std::move(config)) { config_.validate(); }
    [[nodiscard]] Result run() const { return hot_path::run(config_); }
};
inline void write_result(std::ostream& out, const Result& r) {
    out << std::setprecision(12) << "{\"schema\":\"hot_path_cache_v1\",\"queue_config\":";
    write_config(out,r.config.queue);
    out << ",\"transport\":\"" << (r.config.baseline ? "baseline" : name(r.config.queue.transport))
        << "\",\"scenario\":\"" << scenario_name(r.config.scenario)
        << "\",\"measurement\":\"" << (r.config.grouped ? "groups" : r.config.sampled ? "sampled" : "throughput")
        << "\",\"business_kernel\":\"" << (r.config.arithmetic ? "arithmetic" : "cache_sensitive")
        << "\",\"business_bytes\":" << r.config.business_bytes << ",\"business_accesses\":" << r.config.business_accesses
        << ",\"business_warmup_steps\":" << r.config.business_warmup_steps
        << ",\"preparation_rounds\":" << r.config.preparation_rounds
        << ",\"full_oracle\":" << (r.config.full_oracle ? "true" : "false");
#define FIELD(x) out << ",\"" #x "\":" << r.x
    FIELD(producer_seconds); FIELD(drain_seconds); FIELD(callback_seconds); FIELD(m_business_per_second);
    FIELD(process_cpu_seconds); FIELD(peak_rss_mib); FIELD(backpressured_messages); FIELD(failed_attempts);
    FIELD(formal_checksum); FIELD(business_checksum); FIELD(received_checksum); FIELD(callback_checksum);
    FIELD(consumed); FIELD(oracle_checked); FIELD(preparation_messages); FIELD(completed_rounds);
    FIELD(source_pressure_events); FIELD(copy_memory_full_events); FIELD(minor_faults_timed_process);
    FIELD(group_samples); FIELD(group_tail_iterations);
    FIELD(copied_while_paused_bytes); FIELD(max_paused_backlog_bytes); FIELD(pause_confirmed_rounds);
    FIELD(actual_capacity); FIELD(samples); FIELD(migrated_samples); FIELD(body_mean_ns); FIELD(timer_quantum_ns);
#undef FIELD
    out << ",\"timer_name\":\"" << r.timer_name << "\",\"qos_applied\":" << (r.qos_applied ? "true" : "false")
        << ",\"hard_affinity\":" << (r.hard_affinity ? "true" : "false") << ",\"body_ns\":";
    write_quantiles(out,r.body_ns); out << ",\"iteration_ns\":"; write_quantiles(out,r.iteration_ns);
    out << ",\"group_average_iteration_ns\":"; write_quantiles(out,r.group_average_iteration_ns);
    out << ",\"timer_overhead_ns\":"; write_quantiles(out,r.timer_overhead_ns); out << ",\"verified\":true}";
}
} // namespace TwoBufQueue::performance::hot_path
