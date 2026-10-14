#include "../tunable/parameters.hpp"
#include "block_size_round_budget.hpp"
#include <iostream>

#if !defined(BRQ_ENABLE_POOL_DEPTH_STATS) || !defined(BRQ_BLOCK_SIZE_ONLY)
#error Build this target with BRQ_ENABLE_POOL_DEPTH_STATS and BRQ_BLOCK_SIZE_ONLY
#endif

using namespace TwoBufQueue::performance;
namespace {
constexpr std::size_t gib = std::size_t{1024} * 1024 * 1024;
constexpr auto max_publish = (std::numeric_limits<std::uint32_t>::max)();
struct BlockSizeOptions {
    std::size_t reader_block_multiplier = 10;
    std::size_t writer_cache_bytes = 2 * gib;
    std::size_t reader_cache_bytes = 2 * gib;
    std::size_t initial_pool_blocks = 2;
    bool reader_cache_fixed = true;
};

PerformanceConfig defaults() {
    PerformanceConfig c;
    c.payload_bytes = 32;
    c.messages = 10'000'000;
    c.warmup_messages = 50'000;
    c.callback_iterations = 0;
    c.publish_count = 1;
    c.source.block_size = 1024;
    c.source.backpressure.mode = TwoBufQueue::BackpressureMode::Drop;
    c.output.output_mode = TwoBufQueue::HandoffMode::SPSC;
    c.output.output_frame_capacity = 1024;
    c.output.read_config.is_scan_current = true;
    c.output.read_config.idle_rescan_interval = std::chrono::milliseconds{1};
    c.output.read_config.idle_rescan_max_interval = std::chrono::milliseconds{1};
    c.output.read_config.work_wait_min_interval = std::chrono::milliseconds{1};
    c.output.read_config.finish_retry_interval = std::chrono::milliseconds{1};
    c.output.read_config.finish_timeout = std::chrono::milliseconds{5000};
#if defined(__APPLE__)
    c.producer_qos = ProducerQos::user_interactive;
#endif
    c.round_storage_bytes = 0; // Resolve to 80% of the actual reader pool after configuring blocks.
    c.prepared_input_limit_bytes = 2*gib; // Separate input storage, not queue capacity.
    c.measure_callback_time = true;
    c.prefault_producer_buffers = true;
    c.timeout_seconds = 180;
    return c;
}

void configure_pools(PerformanceConfig& c, const BlockSizeOptions& o) {
    if (!o.reader_block_multiplier || !o.initial_pool_blocks || !c.source.block_size ||
        c.source.block_size > o.writer_cache_bytes ||
        c.source.block_size > (std::numeric_limits<std::size_t>::max)()/o.reader_block_multiplier)
        throw std::invalid_argument("invalid block multiplier, source block or writer cache limit");
    if (!o.reader_cache_bytes || c.source.block_size > o.reader_cache_bytes)
        throw std::invalid_argument("reader fixed cache must fit one writer block");
    const auto multiplied = c.source.block_size * o.reader_block_multiplier;
    c.output.output_block_size = o.reader_cache_fixed ? std::min(multiplied,o.reader_cache_bytes) : multiplied;
    const auto reader_limit = o.reader_cache_fixed ? o.reader_cache_bytes : std::max(o.reader_cache_bytes,c.output.output_block_size);
    if (o.writer_cache_bytes > (std::numeric_limits<std::size_t>::max)()-reader_limit)
        throw std::invalid_argument("combined pool limits overflow size_t");
    c.source.pool_max = o.writer_cache_bytes/c.source.block_size;
    c.output.output_pool_max = reader_limit/c.output.output_block_size;
    c.source.pool_min = std::min(o.initial_pool_blocks,c.source.pool_max);
    c.output.output_pool_min = std::min(o.initial_pool_blocks,c.output.output_pool_max);
    // Do not make the handoff queue an earlier limit than the resource pool.
    c.source.queue_capacity = c.source.pool_max + 2;
    c.output.output_queue_capacity = c.output.output_pool_max + 2;
    c.buffer_budget_bytes = o.writer_cache_bytes + reader_limit;
    const auto round_limit = block_size_round_limit(c.output.output_pool_max*c.output.output_block_size);
    if (!c.round_storage_bytes) c.round_storage_bytes = round_limit;
    if (c.round_storage_bytes > round_limit)
        throw std::invalid_argument("round exceeds 80% of actual reader pool capacity");
    if (c.transport != Transport::two_buf_queue || c.payload_bytes != 32 ||
        (c.publish_count != 1 && c.publish_count != max_publish) ||
        c.use_write_session ||
        c.output.output_mode == TwoBufQueue::HandoffMode::Sync ||
        (c.mode != MeasureMode::throughput && c.mode != MeasureMode::write_latency) ||
        !c.prepared_input || !c.variable_payload || !c.round_storage_bytes)
        throw std::invalid_argument("requires asynchronous ordinary 32 B prepared writes, publish_count=1 or UINT32_MAX and producer rounds");
    c.validate();
}

void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
void self_test() {
    struct Resource { bool reset(std::uint64_t) noexcept { return true; } };
    TwoBufQueue::WarmPool<Resource> pool(2,4);
    pool.begin_depth_stats();
    auto a = pool.get(), b = pool.get();
    require(a && b,"initial pool acquisitions");
    require(a.recycle(),"first recycle");
    auto c = pool.get(), d = pool.get();
    require(c && d,"reused and newly allocated resources");
    const auto stats = pool.end_depth_stats();
    require(stats.samples == 4 && stats.in_use_sum == 8 && stats.max_in_use == 3 &&
            stats.resource_sum == 9 && stats.max_resources == 3 && stats.free_sum == 1,
            "pool depth must be sampled atomically after each successful acquisition");
    require(b.recycle() && c.recycle() && d.recycle(),"recycle all resources");
    auto ignored = pool.get();
    require(ignored && pool.end_depth_stats().samples == 4,"sampling stops when disabled");
    require(ignored.recycle(),"recycle disabled sample");
    pool.begin_depth_stats();
    auto e = pool.get(), f = pool.get(), g = pool.get(), h = pool.get();
    require(e && f && g && h && !pool.get(),"finite pool limit");
    require(pool.end_depth_stats().samples == 4,"failed get must not be counted as a switch");
    require(e.recycle() && f.recycle() && g.recycle() && h.recycle(),"release finite pool");

    require(block_size_round_limit(100) == 80 && block_size_round_limit(101) == 80,
            "round limit floors 80 percent without overflowing");
    require(block_size_reader_capacity(8*1024*1024,10,2*gib) == 2000ULL*1024*1024,
            "reader capacity counts only complete blocks");
    auto automatic = defaults();
    configure_pools(automatic,BlockSizeOptions{});
    require(automatic.round_storage_bytes == block_size_round_limit(
                automatic.output.output_block_size*automatic.output.output_pool_max) &&
            automatic.round_messages() > automatic.messages,
            "ten million 32 B records fit one automatic round");
    auto invalid_round = automatic;
    ++invalid_round.round_storage_bytes;
    bool rejected_round = false;
    try { configure_pools(invalid_round,BlockSizeOptions{}); }
    catch (const std::invalid_argument&) { rejected_round = true; }
    require(rejected_round,"round above reader 80 percent must be rejected");

    auto config = defaults();
    config.messages = 20'000;
    config.warmup_messages = 100;
    config.round_storage_bytes = 64 * 1024;
    config.prepared_input_limit_bytes = 64 * 1024;
    config.verify_payload = true;
    config.producer_qos = ProducerQos::unchanged;
    BlockSizeOptions options{4,128 * 1024,128 * 1024,2};
    configure_pools(config,options);
    for (const bool scan : {true,false}) for (const auto mode : {MeasureMode::throughput,MeasureMode::write_latency}) {
        config.output.read_config.is_scan_current = scan;
        config.publish_count = scan ? 1 : max_publish;
        config.mode = mode;
        const auto result = PerformanceTest(config).run();
        require(result.consumed == config.messages && result.pool_depth_stats_available &&
                result.pool_depth.writer.samples > 0 && result.pool_depth.reader.samples > 0 &&
                result.pool_depth.writer.max_in_use <= config.source.pool_max &&
                result.pool_depth.reader.max_in_use <= config.output.output_pool_max,
                "full FIFO/payload validation and both pool samples");
    }
    config.source.block_size = gib;
    options = {};
    configure_pools(config,options);
    require(config.output.output_block_size == 2 * gib && config.output.output_pool_max == 1 &&
            config.source.pool_max == 2,"fixed reader cache caps the multiplied block without expanding the pool");
    require(config.output.output_block_size*config.output.output_pool_max <= 2*gib,
            "reader pool never exceeds the fixed limit");
    options.reader_cache_fixed = false;
    configure_pools(config,options);
    require(config.output.output_block_size == 10*gib && config.output.output_pool_max == 1,
            "explicit legacy expanding mode remains available");
    std::cout << "{\"self_test\":\"passed\"}\n";
}
} // namespace

int main(int argc, char** argv) try {
    auto config = defaults();
    BlockSizeOptions options;
    bool print_defaults = false;
    bool publish_explicit = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (key == "--self-test") { self_test(); return 0; }
        if (key == "--help") {
            std::cout << "test_per_writer_block_size --source_block_size bytes [--reader_block_multiplier 10] ...\n"
                "One isolated run using the existing PerformanceTest measurement loop.\n"
                "Use benchmarks/block_size/test_per_writer_block_size.py for the 1 KiB..1 GiB sweep and three-run averages.\n"
                "Additional options: --writer_cache_bytes, --reader_cache_bytes, --initial_pool_blocks, --is_scan_current true|false, --publish_count 1|4294967295.\n"
                "Without --publish_count, disabled scanning selects UINT32_MAX.\n"
                "The reader limit is fixed by default; multiplied blocks are capped at that limit.\n"
                "Use --reader_cache_fixed false only to reproduce the legacy expanding mode.\n"
                "Standard benchmark parameters, --defaults, --build-info and --self-test are supported.\n";
            return 0;
        }
        if (key == "--defaults") { print_defaults = true; continue; }
        if (key == "--build-info") {
            std::cout << "{\"compiler\":\""
#if defined(__clang__) || defined(__GNUC__)
                << __VERSION__
#elif defined(_MSC_VER)
                << "MSVC " << _MSC_FULL_VER
#else
                << "unknown"
#endif
                << "\",\"optimized\":"
#if defined(__OPTIMIZE__) || (defined(_MSC_VER) && defined(NDEBUG))
                << "true"
#else
                << "false"
#endif
                << ",\"sanitized\":"
#if defined(__clang__)
                << (__has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer) ? "true" : "false")
#elif defined(__SANITIZE_ADDRESS__)
                << "true"
#else
                << "false"
#endif
                << ",\"pool_depth_instrumented\":true,\"payload_bytes\":32,\"supported_publish_counts\":[1,4294967295]}\n";
            return 0;
        }
        if (!key.starts_with("--") || i+1 == argc) throw std::invalid_argument("expected --parameter value");
        const std::string_view value = argv[++i];
        if (key == "--reader_block_multiplier") assign_parameter(options.reader_block_multiplier,value);
        else if (key == "--writer_cache_bytes") assign_parameter(options.writer_cache_bytes,value);
        else if (key == "--reader_cache_bytes") assign_parameter(options.reader_cache_bytes,value);
        else if (key == "--initial_pool_blocks") assign_parameter(options.initial_pool_blocks,value);
        else if (key == "--reader_cache_fixed") assign_parameter(options.reader_cache_fixed,value);
        else {
            set_parameter(config,key.substr(2),value);
            if (key == "--publish_count") publish_explicit = true;
        }
    }
    if (!publish_explicit && !config.output.read_config.is_scan_current) config.publish_count = max_publish;
    configure_pools(config,options);
    if (print_defaults) { write_config(std::cout,config); std::cout << '\n'; return 0; }
    const auto result = PerformanceTest(config).run();
    std::cout << std::setprecision(12) << "{\"block_size_test\":{\"reader_block_multiplier\":"
        << options.reader_block_multiplier << ",\"initial_pool_blocks\":" << options.initial_pool_blocks
        << ",\"writer_cache_limit_bytes\":" << options.writer_cache_bytes
        << ",\"reader_cache_fixed\":" << (options.reader_cache_fixed ? "true" : "false")
        << ",\"reader_cache_requested_bytes\":" << options.reader_cache_bytes
        << ",\"reader_cache_effective_limit_bytes\":"
        << (options.reader_cache_fixed ? options.reader_cache_bytes : std::max(options.reader_cache_bytes,config.output.output_block_size)) << "},\"result\":";
    write_result(std::cout,result);
    std::cout << "}\n";
} catch (const std::exception& error) {
    std::cerr << "block-size benchmark error: " << error.what() << '\n';
    return 2;
}
