#include "hot_path_cache.hpp"
#include <iostream>
using namespace TwoBufQueue::performance;
namespace hp = TwoBufQueue::performance::hot_path;
static void self_test() {
    hp::Config c;
    c.queue.producer_qos = ProducerQos::unchanged;
    c.queue.messages = 8192; c.queue.round_storage_bytes = 4096 * 32;
    c.queue.source.block_size = 1024; c.queue.source.pool_min = c.queue.source.pool_max = 4;
    c.queue.output.output_block_size = 1024; c.queue.output.output_pool_min = c.queue.output.output_pool_max = 256;
    c.queue.output.output_queue_capacity = 258;
    c.queue.spsc_capacity = 8192; c.queue.spsc_buffer_bytes = 0;
    c.business_bytes = 8192; c.business_accesses = 4; c.business_warmup_steps = 200;
    c.preparation_rounds = 2; c.full_oracle = true;
    std::uint64_t expected = 0;
    for (const auto t : {Transport::two_buf_queue,Transport::readerwriterqueue,Transport::rigtorp,Transport::atomic_queue}) {
        c.queue.transport = t;
        for (const auto s : {hp::Scenario::paused,hp::Scenario::slow,hp::Scenario::fast}) {
            c.scenario = s; c.queue.callback_iterations = s == hp::Scenario::slow ? 4 : 0;
            c.sampled = true; c.queue.sample_every = 16;
            const auto r = hp::run(c);
            if (r.consumed != c.queue.messages || r.oracle_checked != c.queue.messages || !r.samples || r.completed_rounds != 2)
                throw std::runtime_error("phase/sample/oracle contract");
            if (expected && expected != r.business_checksum) throw std::runtime_error("business differs between transports");
            expected = r.business_checksum;
        }
    }
    c.sampled = false; c.grouped = true; c.queue.transport = Transport::two_buf_queue;
    const auto grouped = hp::run(c);
    if (grouped.group_samples != 8 || grouped.group_tail_iterations != 192 || grouped.business_checksum != expected)
        throw std::runtime_error("group/tail/business contract");
    c.baseline = true; c.full_oracle = false;
    if (hp::run(c).business_checksum != expected) throw std::runtime_error("baseline business differs");
    c.queue.round_storage_bytes = 256 * 1024;
    bool rejected = false;
    try { c.validate(); } catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected) throw std::runtime_error("80 percent capacity contract");
    std::cout << "hot-path cache contracts passed: all transports/scenarios, oracle, checksum, phases, samples, round bound\n";
}
int main(int argc,char** argv) {
    try {
        hp::Config c;
        for (int i=1; i<argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--self-test") { self_test(); return 0; }
            if (arg == "--help") {
                std::cout << "test_hot_path_cache --transport baseline|two_buf_queue|readerwriterqueue|rigtorp|atomic_queue\n"
                  " --scenario paused|slow|fast --measurement throughput|groups|sampled --business_bytes N\n"
                  " --business_accesses N --business_kernel cache_sensitive|arithmetic --preparation_rounds N\n"
                  " --full_oracle true|false plus all tunable PerformanceConfig --key value options.\n"; return 0;
            }
            if (!arg.starts_with("--") || i+1 == argc) throw std::invalid_argument("expected --key value");
            const auto key = arg.substr(2); const std::string_view value(argv[++i]);
            if (key == "transport" && value == "baseline") c.baseline = true;
            else if (key == "measurement") {
                if (value != "throughput" && value != "sampled" && value != "groups") throw std::invalid_argument("invalid measurement");
                c.sampled = value == "sampled"; c.grouped = value == "groups";
            } else if (key == "business_kernel") {
                if (value != "arithmetic" && value != "cache_sensitive") throw std::invalid_argument("invalid business kernel");
                c.arithmetic = value == "arithmetic";
            } else if (key == "scenario") {
                if (value == "paused") c.scenario = hp::Scenario::paused;
                else if (value == "slow") c.scenario = hp::Scenario::slow;
                else if (value == "fast") c.scenario = hp::Scenario::fast;
                else throw std::invalid_argument("invalid scenario");
            } else if (key == "business_bytes") assign_parameter(c.business_bytes,value);
            else if (key == "business_accesses") assign_parameter(c.business_accesses,value);
            else if (key == "business_warmup_steps") assign_parameter(c.business_warmup_steps,value);
            else if (key == "preparation_rounds") assign_parameter(c.preparation_rounds,value);
            else if (key == "full_oracle") assign_parameter(c.full_oracle,value);
            else set_parameter(c.queue,key,value);
        }
        hp::write_result(std::cout,hp::HotPathCacheTest(c).run()); std::cout << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "hot-path cache: " << e.what() << '\n'; return 1; }
}
