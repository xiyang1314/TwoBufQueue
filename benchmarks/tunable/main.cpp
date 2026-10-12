#include "parameters.hpp"
#include <iostream>
using namespace TwoBufQueue::performance;
int main(int argc, char** argv) try {
    PerformanceConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help") {
            std::cout << "brq_tunable_benchmark --parameter value ...\n"
                "--defaults emits all constructor parameters as JSON.\n"
                "Modes: throughput, write_latency (group-average quantiles, --latency_batch_size 1000), single_write_latency (one call), call_batch (overall reference mean).\n";
            return 0;
        }
        if (argument == "--build-info") {
            std::cout << "{\"compiler\":\""
#if defined(__clang__)
                << "Clang " << __clang_version__
#elif defined(__GNUC__)
                << "GCC " << __VERSION__
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
#if defined(__SANITIZE_ADDRESS__)
                << "true"
#elif defined(__clang__)
                << (__has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer) ? "true" : "false")
#else
                << "false"
#endif
                << ",\"ordinary_writer_readback\":" << (TwoBufQueue::Writer<1, false>::ordinary_write_readback ? "true" : "false")
                << ",\"session_readback\":false"
                << ",\"payload_copy_strategy\":\"" << payload_copy_strategy << "\"}\n";
            return 0;
        }
        if (argument == "--defaults") { write_config(std::cout,config); std::cout << '\n'; return 0; }
        if (!argument.starts_with("--") || i+1 == argc) throw std::invalid_argument("expected --parameter value");
        set_parameter(config,argument.substr(2),argv[++i]);
    }
    const auto result = PerformanceTest(config).run();
    write_result(std::cout,result); std::cout << '\n';
} catch (const std::exception& error) {
    std::cerr << "benchmark error: " << error.what() << '\n'; return 2;
}
