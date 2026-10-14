// Single-thread control: the real ordinary Writer, with synchronous recycling
// of completed handoffs. No read worker, layer-2 copy, business callback or watchdog thread.
#include "../tunable/parameters.hpp"
#include "block_size_round_budget.hpp"
#include <iostream>

#if !defined(BRQ_ENABLE_POOL_DEPTH_STATS)
#error "Build with BRQ_ENABLE_POOL_DEPTH_STATS=ON"
#endif

namespace {
using namespace TwoBufQueue;
using namespace TwoBufQueue::performance;
namespace perf_detail = TwoBufQueue::performance::detail;
constexpr std::uint32_t max_publish = (std::numeric_limits<std::uint32_t>::max)();
constexpr std::size_t payload_bytes = 32;
using Payload32 = Payload<payload_bytes>;

struct Config {
    PerformanceConfig workload;
    std::size_t writer_cache_bytes = 2ULL * 1024 * 1024 * 1024;
    std::size_t initial_pool_blocks = 2;
    std::size_t reader_cache_bytes = 2ULL*1024*1024*1024;
    std::size_t reader_block_multiplier = 10; // Capacity reference only; no reader is instantiated.
    Config() {
        workload.payload_bytes = payload_bytes;
        workload.messages = 10'000'000;
        workload.source.block_size = 1024;
        workload.publish_count = 1;
        workload.round_storage_bytes = 0;
        workload.prepared_input_limit_bytes = 2ULL*1024*1024*1024;
        workload.prefault_producer_buffers = true;
        workload.timeout_seconds = 180;
#if defined(__APPLE__)
        workload.producer_qos = ProducerQos::user_interactive;
#endif
    }
    void resolve_round_limit() {
        const auto limit = block_size_round_limit(block_size_reader_capacity(
            workload.source.block_size,reader_block_multiplier,reader_cache_bytes));
        if (!workload.round_storage_bytes) workload.round_storage_bytes = limit;
        if (workload.round_storage_bytes > limit)
            throw std::invalid_argument("round exceeds 80% of reader capacity reference");
    }
    void validate() const {
        const auto& c = workload;
        if (c.payload_bytes != payload_bytes || !c.messages ||
            c.messages > (std::uint64_t{1} << 60) || c.warmup_messages > (std::uint64_t{1} << 60) ||
            c.messages > (std::numeric_limits<std::uint64_t>::max)() - c.warmup_messages)
            throw std::invalid_argument("invalid 32 B message counts");
        if (c.source.block_size < payload_bytes || c.source.block_size % payload_bytes ||
            writer_cache_bytes / c.source.block_size < 2 || !initial_pool_blocks ||
            initial_pool_blocks > writer_cache_bytes / c.source.block_size)
            throw std::invalid_argument("blocks must hold whole 32 B records; the pool must fit at least two blocks");
        if (c.publish_count != 1 && c.publish_count != max_publish)
            throw std::invalid_argument("publish_count must be 1 or UINT32_MAX");
        if (c.mode != MeasureMode::throughput && c.mode != MeasureMode::write_latency)
            throw std::invalid_argument("mode must be throughput or write_latency");
        if (!c.round_storage_bytes || !c.round_messages() || !c.latency_batch_size || !c.max_samples ||
            c.timer_calibration_samples < 100 || !c.timeout_seconds || c.producer_cpu < -1)
            throw std::invalid_argument("invalid round, latency, timer, timeout or CPU configuration");
        if (c.mode == MeasureMode::write_latency &&
            std::min(c.messages,c.round_messages()) < c.latency_batch_size)
            throw std::invalid_argument("a latency round must fit one complete group");
        if (std::min(c.messages,c.round_messages()) > c.prepared_input_limit_bytes / payload_bytes)
            throw std::invalid_argument("prepared round exceeds prepared_input_limit_bytes");
    }
};

// This runs on the writer's own thread, only from the existing cold-path
// notification. It never calls a producer API and never reads a current block.
// Validation is a separate run; measured runs merely advance the read position
// required by the existing completion/recycle contract, without loading payloads.
struct Recycler {
    WriterQueue& queue;
    bool verify;
    bool valid = true;
    std::uint64_t expected = 0, retired_messages = 0, completed_blocks = 0;
    std::uint64_t notification_calls = 0, empty_notifications = 0;
    void drain() noexcept {
        bool completed = false;
        while (auto record = queue.try_pop()) {
            if (auto* block = record.get()) {
                completed = true;
                const auto bytes = block->write_pos();
                if (bytes % payload_bytes) valid = false;
                const auto count = bytes / payload_bytes;
                if (verify) {
                    for (std::size_t i = 0; i < count; ++i) {
                        for (std::size_t word = 0; word < payload_bytes / 8; ++word) {
                            std::uint64_t value;
                            std::memcpy(&value,block->data() + i * payload_bytes + word * 8,8);
                            const auto wanted = word == 0 ? expected : perf_detail::payload_word(expected,word);
                            if (value != wanted) valid = false;
                        }
                        ++expected;
                    }
                }
                retired_messages += count;
                ++completed_blocks;
                if (!block->consume(bytes)) std::terminate();
            }
            queue.finish(record); // Original lifecycle callbacks return the block to its pool.
        }
        if (!completed) ++empty_notifications;
    }
    static void notify(void* context) noexcept {
        auto& self = *static_cast<Recycler*>(context);
        ++self.notification_calls;
        self.drain();
    }
};

struct Result {
    PerformanceResult scheduling;
    PoolDepthStats depth;
    WriterBackpressureStats pressure;
    double producer_seconds = 0, preparation_seconds = 0, round_completion_seconds = 0;
    double wall_seconds = 0, cpu_seconds = 0, peak_rss_mib = 0;
    std::uint64_t retired_messages = 0, completed_blocks = 0, natural_switches = 0, rounds = 0;
    std::uint64_t notification_calls = 0, empty_notifications = 0, latency_tail_writes = 0;
    std::uint64_t migrated_samples = 0;
    std::size_t latency_samples = 0, prepared_input_bytes = 0;
    Quantiles write_ns{}, timer_overhead_ns{};
    std::string timer;
    double timer_quantum_ns = 0;
    bool verified = false, all_released = false;
};

template<std::uint32_t TPublish>
Result run(const Config& config) {
    const auto& c = config.workload;
    Result result;
    perf_detail::ProducerAffinity affinity(c.producer_cpu);
    perf_detail::ProducerScheduling scheduling(c.producer_qos,result.scheduling);
    const auto maximum = config.writer_cache_bytes / c.source.block_size;
    WriterPool pool(config.initial_pool_blocks,maximum,SpscBlockFactory<SpscWriteBlock>{c.source.block_size});
    auto queue = make_writer_queue(pool,maximum + 2);
    Recycler recycler{queue,c.verify_payload};
    queue.set_read_available_callback(Recycler::notify,&recycler);
    Writer<TPublish,false> writer(&queue,0,{.mode = BackpressureMode::Drop});
    recycler.drain(); // Acknowledge the initial {empty,current} handoff, not its payload.
    if (c.prefault_producer_buffers) {
        std::vector<WriterPool::Loan> loans;
        std::memset(writer.source_block()->data(),0,c.source.block_size);
        for (std::size_t i = 1; i < config.initial_pool_blocks; ++i) {
            auto loan = pool.get();
            if (!loan) std::terminate();
            std::memset(loan->data(),0,c.source.block_size);
            loans.push_back(loan);
        }
        for (auto loan : loans)
            if (!loan->to_write_finished() || !loan->to_leave_writer() || !loan->finish()) std::terminate();
    }
    Payload32 warmup;
    for (std::uint64_t i = 0; i < c.warmup_messages; ++i) {
        perf_detail::set_payload(warmup,i,true);
        if (!writer.write(warmup.bytes)) std::terminate();
    }
    writer.finish_block(); // Same empty starting state for both publication policies.
    if (!recycler.valid || recycler.retired_messages != c.warmup_messages || !pool.all_released())
        throw std::runtime_error("warmup/recycle validation failed");
    recycler.retired_messages = recycler.completed_blocks = 0;
    recycler.notification_calls = recycler.empty_notifications = 0;

    ProbeClock clock(c); // Same native tick clock and empty-probe calibration as the pipeline benchmark.
    result.timer = clock.label(); result.timer_quantum_ns = clock.quantum_ns();
    result.timer_overhead_ns = clock.overhead();
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(c.max_samples,c.messages/c.latency_batch_size)));
    std::vector<Payload32> prepared;
    const auto prepare = [&](std::uint64_t first, std::uint64_t count) {
        const auto begin = std::chrono::steady_clock::now();
        prepared.resize(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < prepared.size(); ++i) {
            prepared[i].bytes.fill(std::byte{0xa5});
            perf_detail::set_payload(prepared[i],c.warmup_messages + first + i,true);
        }
        result.prepared_input_bytes = std::max(result.prepared_input_bytes,prepared.size()*payload_bytes);
        result.preparation_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    };
    prepare(0,std::min(c.messages,c.round_messages()));
    pool.begin_depth_stats();
    const auto cpu_begin = benchmark::cpu_seconds();
    const auto wall_begin = std::chrono::steady_clock::now();
    for (std::uint64_t first = 0; first < c.messages;) {
        const auto count = std::min(c.messages-first,c.round_messages());
        if (first) prepare(first,count);
        const auto begin = std::chrono::steady_clock::now();
        const auto* input = prepared.data();
        const auto* const finish = input + count;
        if (c.mode == MeasureMode::throughput) {
            // Identical ordinary-write input loop, no per-message handoff polling.
            for (; input != finish; ++input)
                if (!writer.write(input->bytes)) std::terminate();
        } else {
            while (input != finish) {
                const auto group = std::min<std::size_t>(c.latency_batch_size,finish-input);
                const auto* const group_end = input + group;
                const bool sampled = group == c.latency_batch_size && samples.size() < c.max_samples;
                ProbeClock::Stamp a{};
                if (sampled) a = clock.read();
                for (; input != group_end; ++input)
                    if (!writer.write(input->bytes)) std::terminate();
                if (sampled) {
                    const auto b = clock.read();
                    if (a.cpu == b.cpu && b.ticks >= a.ticks) samples.push_back(clock.elapsed(a,b)/group);
                    else ++result.migrated_samples;
                }
                if (group != c.latency_batch_size) result.latency_tail_writes += group;
            }
        }
        const auto end = std::chrono::steady_clock::now();
        result.producer_seconds += std::chrono::duration<double>(end-begin).count();
        writer.finish_block(); // Tail handoff/recycling outside active write timing, for both policies.
        result.round_completion_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now()-end).count();
        ++result.rounds;
        first += count;
        if (std::chrono::steady_clock::now()-wall_begin > std::chrono::seconds{c.timeout_seconds})
            throw std::runtime_error("single-thread benchmark timed out");
    }
    writer.finish();
    result.depth = pool.end_depth_stats();
    result.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-wall_begin).count();
    result.cpu_seconds = benchmark::cpu_seconds()-cpu_begin;
    result.peak_rss_mib = benchmark::peak_rss_mib();
    result.pressure = writer.backpressure_stats();
    result.retired_messages = recycler.retired_messages;
    result.completed_blocks = recycler.completed_blocks;
    result.natural_switches = recycler.completed_blocks-result.rounds;
    result.notification_calls = recycler.notification_calls;
    result.empty_notifications = recycler.empty_notifications;
    result.verified = c.verify_payload && recycler.valid && recycler.expected == c.warmup_messages+c.messages;
    result.all_released = pool.all_released() && queue.pending_count() == 0 && !queue.current();
    result.latency_samples = samples.size();
    result.write_ns = quantiles(std::move(samples));
    queue.set_read_available_callback(nullptr,nullptr);
    if (!result.all_released || !recycler.valid || result.retired_messages != c.messages ||
        (c.verify_payload && !result.verified) || result.pressure.pressure_events)
        throw std::runtime_error("formal write/recycle validation failed");
    return result;
}

Result dispatch(Config& config) {
    config.resolve_round_limit();
    config.validate();
    return config.workload.publish_count == 1 ? run<1>(config) : run<max_publish>(config);
}

void write_json(std::ostream& out, const Config& config, const Result& r) {
    const auto& c = config.workload;
    out << std::setprecision(12) << "{\"config\":{\"payload_bytes\":32,\"source_block_size\":" << c.source.block_size
        << ",\"writer_cache_bytes\":" << config.writer_cache_bytes << ",\"initial_pool_blocks\":" << config.initial_pool_blocks
        << ",\"pool_max_blocks\":" << config.writer_cache_bytes/c.source.block_size
        << ",\"messages\":" << c.messages << ",\"warmup_messages\":" << c.warmup_messages
        << ",\"publish_count\":" << c.publish_count << ",\"mode\":\"" << name(c.mode)
        << "\",\"verify_payload\":" << (c.verify_payload ? "true" : "false")
        << ",\"reader_cache_reference_bytes\":" << config.reader_cache_bytes
        << ",\"reader_block_multiplier\":" << config.reader_block_multiplier
        << ",\"reader_capacity_reference_bytes\":" << block_size_reader_capacity(
            c.source.block_size,config.reader_block_multiplier,config.reader_cache_bytes)
        << ",\"round_storage_bytes\":" << c.round_storage_bytes << ",\"latency_batch_size\":" << c.latency_batch_size
        << ",\"producer_cpu\":" << c.producer_cpu << ",\"producer_qos\":\""
        << (c.producer_qos == ProducerQos::user_interactive ? "user_interactive" : "unchanged")
        << "\",\"prefault_producer_buffers\":" << (c.prefault_producer_buffers ? "true" : "false")
        << "},\"single_thread\":true,\"reader_threads\":0,\"callback_threads\":0"
        << ",\"timing_scope\":\"ordinary_write_with_immediate_completed_block_recycle\""
        << ",\"producer_seconds\":" << r.producer_seconds
        << ",\"m_messages_per_second\":" << c.messages/r.producer_seconds/1e6
        << ",\"payload_gb_per_second\":" << c.messages/r.producer_seconds*payload_bytes/1e9
        << ",\"input_preparation_seconds\":" << r.preparation_seconds
        << ",\"prepared_input_bytes\":" << r.prepared_input_bytes
        << ",\"round_completion_seconds\":" << r.round_completion_seconds
        << ",\"wall_seconds\":" << r.wall_seconds << ",\"cpu_seconds\":" << r.cpu_seconds
        << ",\"peak_rss_mib\":" << r.peak_rss_mib << ",\"retired_messages\":" << r.retired_messages
        << ",\"completed_blocks\":" << r.completed_blocks << ",\"natural_switches\":" << r.natural_switches
        << ",\"rounds\":" << r.rounds << ",\"notification_calls\":" << r.notification_calls
        << ",\"empty_notifications\":" << r.empty_notifications << ",\"failed_write_calls\":0"
        << ",\"writer_pressure_events\":" << r.pressure.pressure_events
        << ",\"all_released\":" << (r.all_released ? "true" : "false")
        << ",\"verified\":" << (r.verified ? "true" : "false")
        << ",\"producer_qos_applied\":" << (r.scheduling.producer_qos_applied ? "true" : "false")
        << ",\"producer_hard_affinity\":" << (c.producer_cpu >= 0 ? "true" : "false")
        << ",\"latency_samples\":" << r.latency_samples << ",\"migrated_samples\":" << r.migrated_samples
        << ",\"latency_tail_writes\":" << r.latency_tail_writes
        << ",\"timer\":\"" << r.timer << "\",\"timer_quantum_ns\":" << r.timer_quantum_ns
        << ",\"timer_overhead_ns\":";
    write_quantiles(out,r.timer_overhead_ns);
    out << ",\"batch_average_write_ns\":"; write_quantiles(out,r.write_ns);
    out << ",\"pool_depth\":{\"writer\":"; write_pool_depth(out,r.depth,c.source.block_size);
    out << "}}\n";
}

void self_test() {
    for (const auto publish : {1u,max_publish}) for (const auto block : {1024u,2048u,8192u}) {
        Config c;
        c.workload.messages = 20'013;
        c.workload.warmup_messages = 67;
        c.workload.source.block_size = block;
        c.workload.verify_payload = true;
        c.workload.round_storage_bytes = 3'007*payload_bytes;
        c.workload.timer_calibration_samples = 100;
        c.writer_cache_bytes = block*2;
        c.initial_pool_blocks = 1; // Verify pool grows to two, then reuses rather than accumulating.
        c.workload.publish_count = publish;
        for (const auto mode : {MeasureMode::throughput,MeasureMode::write_latency}) {
            c.workload.mode = mode;
            const auto r = dispatch(c);
            const auto per_block = block/payload_bytes;
            const auto full_rounds = c.workload.messages/c.workload.round_messages();
            const auto tail = c.workload.messages%c.workload.round_messages();
            const auto expected_blocks = full_rounds*((c.workload.round_messages()+per_block-1)/per_block)
                + (tail+per_block-1)/per_block;
            const auto expected_samples = full_rounds*(c.workload.round_messages()/c.workload.latency_batch_size)
                + tail/c.workload.latency_batch_size;
            if (!r.verified || !r.all_released || r.completed_blocks != expected_blocks ||
                r.natural_switches != expected_blocks-r.rounds || r.depth.samples != expected_blocks ||
                r.depth.max_in_use != 2 || r.depth.max_resources != 2 ||
                (mode == MeasureMode::write_latency && r.latency_samples != expected_samples))
                throw std::runtime_error("only-writer benchmark contract failed");
        }
    }
    Config invalid;
    invalid.writer_cache_bytes = invalid.workload.source.block_size;
    bool rejected = false;
    try { invalid.validate(); } catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected) throw std::runtime_error("one-block pool must be rejected");
    std::cout << "only-writer full-payload/FIFO, immediate recycle, two-block pool and latency contracts passed\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        Config config;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--self-test") { self_test(); return 0; }
            if (key == "--build-info") {
                std::cout << "{\"optimized\":"
#if defined(__OPTIMIZE__) || (defined(_MSC_VER) && defined(NDEBUG))
                    << "true"
#else
                    << "false"
#endif
                    << ",\"sanitized\":"
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
                    << "true"
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
                    << "true"
#else
                    << "false"
#endif
#else
                    << "false"
#endif
                    << ",\"pool_depth_instrumented\":true,\"single_thread\":true,\"payload_bytes\":32,"
                       "\"supported_publish_counts\":[1,4294967295]}\n";
                return 0;
            }
            if (key == "--help") {
                std::cout << "test_per_write_block_size_only_writer: 32 B ordinary write, one thread\n"
                    "--source_block_size --writer_cache_bytes --initial_pool_blocks --messages --warmup_messages\n"
                    "--publish_count (1 or 4294967295) --mode (throughput or write_latency) --verify_payload\n"
                    "--round_storage_bytes --latency_batch_size --max_samples --timer --timer_calibration_samples\n"
                    "--producer_cpu --producer_qos --prefault_producer_buffers --timeout_seconds\n"
                    "--self-test --build-info\n";
                return 0;
            }
            if (i+1 == argc || !key.starts_with("--")) throw std::invalid_argument("expected --parameter value");
            const std::string value = argv[++i];
            if (key == "--writer_cache_bytes") assign_parameter(config.writer_cache_bytes,value);
            else if (key == "--initial_pool_blocks") assign_parameter(config.initial_pool_blocks,value);
            else if (key == "--reader_cache_bytes") assign_parameter(config.reader_cache_bytes,value);
            else if (key == "--reader_block_multiplier") assign_parameter(config.reader_block_multiplier,value);
            else {
                const std::string allowed = "|source_block_size|messages|warmup_messages|publish_count|mode|verify_payload|"
                    "round_storage_bytes|latency_batch_size|max_samples|timer|timer_calibration_samples|"
                    "producer_cpu|producer_qos|prefault_producer_buffers|timeout_seconds|prepared_input_limit_bytes|";
                if (allowed.find("|"+key.substr(2)+"|") == std::string::npos)
                    throw std::invalid_argument("unknown parameter: "+key);
                set_parameter(config.workload,key.substr(2),value);
            }
        }
        const auto result = dispatch(config);
        write_json(std::cout,config,result);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
