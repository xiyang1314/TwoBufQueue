#include "tunable/parameters.hpp"
#include <cmath>
#include <iostream>
#include <sstream>

namespace perf = TwoBufQueue::performance;
static void require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}
template<class F> static void invalid(F function) {
    bool rejected = false;
    try { function(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected,"invalid configuration was accepted");
}
struct HeldCallback {
    std::atomic<bool> entered{false}, release{false};
    void operator()(std::int32_t, const std::byte*, std::size_t) noexcept {
        entered.store(true,std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    }
};
template<class TChannel> static void check_pressure_channel(perf::PerformanceConfig config, bool library_stats) {
    HeldCallback sink;
    TChannel channel(config,sink);
    try {
        perf::Payload<8> value{};
        require(channel.push(value),"initial pressure-test write");
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds{3};
        while (!sink.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<deadline)
            std::this_thread::yield();
        require(sink.entered.load(std::memory_order_acquire),"callback gate was not entered");
        bool full = false;
        const auto attempts = std::max<std::size_t>(10000,channel.capacity()+1);
        for (std::size_t i=0;i<attempts;++i) if (!channel.push(value)) { full = true; break; }
        if (!full) throw std::runtime_error("held callback did not fill finite buffers; native capacity="+
            std::to_string(channel.capacity())+", internal stats="+std::to_string(library_stats));
        auto snapshot = channel.pressure_stats();
        require(snapshot.available == library_stats,"unsupported internal stats must be marked unavailable");
        if (library_stats) {
            while ((!snapshot.copy.memory_full_events || !snapshot.copy.memory_backpressured_frames) &&
                   std::chrono::steady_clock::now()<deadline) {
                std::this_thread::yield(); snapshot = channel.pressure_stats();
            }
            require(snapshot.writer.pressure_events && snapshot.writer.dropped_records,
                    "writer source-pressure snapshot missing");
            require(snapshot.copy.memory_full_events && snapshot.copy.memory_backpressured_frames,
                    "copy-to-output pressure snapshot missing");
        }
    } catch (...) { sink.release.store(true,std::memory_order_release); throw; }
    sink.release.store(true,std::memory_order_release);
    channel.finish(); channel.stop(); channel.check();
}
static void check_internal_block_wait(perf::PerformanceConfig config) {
    config.source.backpressure.mode = TwoBufQueue::BackpressureMode::Block;
    config.source.backpressure.spin_count = 0;
    HeldCallback sink;
    perf::TwoBufChannel<1,HeldCallback> channel(config,sink);
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds{3};
    // Release the consumer as soon as the writer records an internal wait.
    std::thread releaser([&] {
        while (!channel.pressure_stats().writer.blocked_events && std::chrono::steady_clock::now()<deadline)
            std::this_thread::yield();
        sink.release.store(true,std::memory_order_release);
    });
    perf::Payload<8> value{};
    std::uint64_t failures = 0;
    for (unsigned i=0; i<10000 && !channel.pressure_stats().writer.blocked_events; ++i)
        if (!channel.push(value)) ++failures;
    releaser.join(); channel.finish(); channel.stop(); channel.check();
    const auto snapshot = channel.pressure_stats();
    require(!failures && snapshot.writer.blocked_events && snapshot.writer.blocked_ticks,
            "successful Block writes must still expose internal waits");
}
template<std::size_t TBytes> static void check_fixed_record_parser() {
    struct Collector {
        std::uint64_t received = 0;
        bool valid = true;
        void operator()(std::int32_t source,const std::byte* bytes,std::size_t size) noexcept {
            std::uint64_t value = ~std::uint64_t{0};
            if (size == TBytes) std::memcpy(&value,bytes,8);
            valid &= source == 7 && size == TBytes && value == received;
            ++received;
        }
    };
    std::array<perf::Payload<TBytes>,3> values{};
    for (std::uint64_t i=0;i<3;++i) std::memcpy(values[i].bytes.data(),&i,8);
    {
        TwoBufQueue::ReadBlock block(512,4);
        require(block.append(7,reinterpret_cast<const std::byte*>(values.data()),2*TBytes,0),"first fixed-size frame");
        require(block.append(7,values[2].bytes.data(),TBytes,0),"second fixed-size frame");
        Collector sink;
        perf::FixedRecordCallback<TBytes,Collector> callback{&sink};
        require(callback(TwoBufQueue::to_data_batch(block)) && sink.valid && sink.received==3,
                "fixed-size parser must preserve message boundaries and FIFO");
    }
    {
        TwoBufQueue::ReadBlock block(512,4);
        require(block.append(7,values[0].bytes.data(),TBytes,0),"valid prefix frame");
        require(block.append(7,values[1].bytes.data(),TBytes-1,0),"malformed suffix frame");
        Collector sink;
        perf::FixedRecordCallback<TBytes,Collector> callback{&sink};
        require(!callback(TwoBufQueue::to_data_batch(block)) && sink.received==0,
                "reject malformed batches before delivering any callbacks");
    }
}
template<std::size_t TBytes, class TFactory>
static void check_complete_copy(perf::PerformanceConfig config, TFactory make_channel) {
    struct Collector {
        std::array<std::array<std::byte,TBytes>,5> copied{};
        std::atomic<std::size_t> received{0};
        bool valid=true;
        void operator()(std::int32_t source,const std::byte* bytes,std::size_t size) noexcept {
            const auto i=received.load(std::memory_order_relaxed);
            if (i>=copied.size() || size!=TBytes || source!=0) valid=false;
            else std::memcpy(copied[i].data(),bytes,TBytes);
            received.store(i+1,std::memory_order_release);
        }
    };
    config.payload_bytes=TBytes;
    Collector sink;
    auto channel=make_channel(config,sink);
    std::array<std::array<std::byte,TBytes>,5> expected{};
    for (std::size_t i=0;i<expected.size();++i) {
        perf::Payload<TBytes> value;
        for (std::size_t byte=0;byte<TBytes;++byte)
            value.bytes[byte]=std::byte{static_cast<unsigned char>((i*17+byte*29+11)&255)};
        expected[i]=value.bytes;
        require(channel->push(value),"copy audit write rejected");
        value.bytes.fill(std::byte{0xff}); // Overwrite source immediately after write returns.
    }
    channel->finish();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{3};
    while (sink.received.load(std::memory_order_acquire)<expected.size() &&
           std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    channel->stop();channel->check();
    require(sink.valid && sink.received==expected.size() && sink.copied==expected,
            "complete-copy audit: bytes/FIFO/lifetime mismatch");
}
template<std::size_t TBytes> static void check_copy_adapters(perf::PerformanceConfig config) {
    using Value=perf::Payload<TBytes>;
    check_complete_copy<TBytes>(config,[]<class Sink>(const auto& cfg,Sink& sink) {
        return std::make_unique<perf::TwoBufChannel<1,Sink,TBytes>>(cfg,sink);
    });
    check_complete_copy<TBytes>(config,[]<class Sink>(const auto& cfg,Sink& sink) {
        return std::make_unique<perf::SpscChannel<Sink,perf::ReaderWriterAdapter<Value>>>(cfg,sink);
    });
    check_complete_copy<TBytes>(config,[]<class Sink>(const auto& cfg,Sink& sink) {
        return std::make_unique<perf::SpscChannel<Sink,perf::ReaderWriterAdapter<Value,true>>>(cfg,sink);
    });
    check_complete_copy<TBytes>(config,[]<class Sink>(const auto& cfg,Sink& sink) {
        return std::make_unique<perf::SpscChannel<Sink,perf::RigtorpAdapter<Value>>>(cfg,sink);
    });
    // Both placement-construction paths must preserve every payload byte and FIFO.
    check_complete_copy<TBytes>(config,[]<class Sink>(const auto& cfg,Sink& sink) {
        return std::make_unique<perf::SpscChannel<Sink,perf::RigtorpAdapter<Value,false>>>(cfg,sink);
    });
    check_complete_copy<TBytes>(config,[]<class Sink>(const auto& cfg,Sink& sink) {
        return std::make_unique<perf::SpscChannel<Sink,perf::AtomicAdapter<Value>>>(cfg,sink);
    });
}
int main() {
    try {
        check_fixed_record_parser<8>(); check_fixed_record_parser<16>();
        check_fixed_record_parser<32>(); check_fixed_record_parser<64>();
        {
            perf::PerformanceResult r;
            unsigned attempts=0, pauses=0;
            perf::detail::retry_failed_write(r,[&] { return ++attempts==3; },[&] { ++pauses; });
            require(r.failed_write_calls==3 && r.backpressured_messages==1 && pauses==3,
                    "one initially failed message / two failed retries counted incorrectly");
            require(r.producer_retry_seconds>=r.producer_retry_max_seconds,"retry duration accounting");
            perf::PerformanceConfig x;
            x.source.block_size=x.output.output_block_size=64;
            x.source.pool_min=x.source.pool_max=x.output.output_pool_min=x.output.output_pool_max=2;
            x.source.queue_capacity=x.output.output_queue_capacity=4;
            x.output.output_mode=TwoBufQueue::HandoffMode::SPSC; x.spsc_capacity=2;
            using V=perf::Payload<8>;
            check_pressure_channel<perf::TwoBufChannel<1,HeldCallback>>(x,true);
            check_pressure_channel<perf::SpscChannel<HeldCallback,perf::RigtorpAdapter<V>>>(x,false);
            check_pressure_channel<perf::SpscChannel<HeldCallback,perf::ReaderWriterAdapter<V>>>(x,false);
            check_pressure_channel<perf::SpscChannel<HeldCallback,perf::AtomicAdapter<V>>>(x,false);
            check_internal_block_wait(x);
        }
        perf::PerformanceConfig c;
        {
            auto copy_config=c;
            copy_config.source.block_size=copy_config.output.output_block_size=512;
            copy_config.source.pool_min=copy_config.source.pool_max=8;
            copy_config.output.output_pool_min=copy_config.output.output_pool_max=8;
            copy_config.source.queue_capacity=copy_config.output.output_queue_capacity=16;
            copy_config.output.output_mode=TwoBufQueue::HandoffMode::SPSC;
            copy_config.spsc_capacity=16;
            check_copy_adapters<8>(copy_config); check_copy_adapters<16>(copy_config);
            check_copy_adapters<32>(copy_config); check_copy_adapters<64>(copy_config);
        }
        invalid([&] { auto x=c; x.prepared_input_limit_bytes=1; x.validate(); });
        {
            auto x = c;
            perf::set_parameter(x,"is_scan_current","false");
            perf::set_parameter(x,"publish_count","4294967295");
            x.validate();
            std::ostringstream metadata; perf::write_config(metadata,x);
            require(!x.output.read_config.is_scan_current && x.publish_count == UINT32_MAX &&
                    metadata.str().find("\"is_scan_current\":false") != std::string::npos,
                    "completed-only configuration must be accepted and recorded");
        }
        invalid([&] { auto x=c; x.prepared_input_limit_bytes=0; x.validate(); });
        {
            auto x=c; perf::set_parameter(x,"rigtorp_write_prefetch","false");
            std::ostringstream metadata; perf::write_config(metadata,x);
            require(!x.rigtorp_write_prefetch &&
                    metadata.str().find("\"rigtorp_write_prefetch\":false")!=std::string::npos,
                    "rigtorp prefetch choice must be parsed and recorded");
        }
        {
            auto x=c; perf::set_parameter(x,"readerwriterqueue_copy_after_zero","true");
            std::ostringstream metadata; perf::write_config(metadata,x);
            require(x.readerwriterqueue_copy_after_zero &&
                    metadata.str().find("\"readerwriterqueue_copy_after_zero\":true")!=std::string::npos,
                    "ReaderWriterQueue copy policy must be parsed and recorded");
        }
        require(c.latency_batch_size == 1000,"default latency group size");
        require(c.prepared_input && c.variable_payload,"write measurements require prepared full payloads");
        require(c.output.output_mode == TwoBufQueue::HandoffMode::MPSC,"default callbacks must be asynchronous");
        invalid([&] { auto x = c; x.measure_callback_time = true; x.validate(); });
        invalid([&] { auto x = c; x.output.output_reader_count = 2; x.validate(); });
        invalid([&] { auto x = c; x.output.output_retry_wait = std::chrono::microseconds{0}; x.validate(); });
        invalid([&] { auto x = c; perf::set_parameter(x,"output_mode","invalid"); });
        invalid([&] { auto x = c; perf::set_parameter(x,"output_reader_mode","invalid"); });
        {
            auto x = c;
            perf::set_parameter(x,"output_mode","sync");
            perf::set_parameter(x,"output_reader_mode","polling");
            perf::set_parameter(x,"output_retry_wait_us","17");
            perf::set_parameter(x,"output_enable_stats","true");
            require(x.output.output_mode == TwoBufQueue::HandoffMode::Sync &&
                    x.output.output_reader_mode == TwoBufQueue::ReaderMode::Polling &&
                    x.output.output_retry_wait.count() == 17 && x.output.output_enable_stats,
                    "output options parser");
            std::ostringstream json; perf::write_config(json,x);
            require(json.str().find("\"output_mode\":\"sync\"") != std::string::npos &&
                    json.str().find("\"output_reader_mode\":\"polling\"") != std::string::npos,
                    "output options reproduction metadata");
        }
        invalid([&] { auto x = c; x.payload_bytes = 12; perf::PerformanceTest test(x); });
        invalid([&] { auto x = c; x.publish_count = 3; x.validate(); });
        invalid([&] { auto x = c; x.output.output_block_size = x.source.block_size-1; x.validate(); });
        invalid([&] { auto x = c; x.source.pool_min = x.source.pool_max+1; x.validate(); });
        invalid([&] { auto x = c; x.sample_every = 0; x.validate(); });
        invalid([&] { auto x = c; x.latency_batch_size = 0; x.validate(); });
        invalid([&] { auto x = c; x.mode = perf::MeasureMode::write_latency; x.messages = 999; x.validate(); });
        invalid([&] { auto x = c; x.mode = static_cast<perf::MeasureMode>(99); x.validate(); });
        invalid([&] { perf::set_parameter(c,"messages","-1"); });
        invalid([&] { perf::set_parameter(c,"messages","12garbage"); });
        invalid([&] { perf::set_parameter(c,"unknown","1"); });
        invalid([&] { perf::set_parameter(c,"verify_payload","maybe"); });
        perf::set_parameter(c,"callback_iterations","7");
        perf::set_parameter(c,"rescan_interval_ns","1000000");
        require(c.callback_iterations == 7 && c.output.read_config.idle_rescan_interval.count() == 1000000,
                "configuration field parser failed");
        std::vector<double> values;
        for (unsigned i = 1000; i; --i) values.push_back(i);
        const auto q = perf::quantiles(values);
        require(q.p50 == 500 && q.p99 == 990 && q.p999 == 999 && q.maximum == 1000,"nearest-rank percentiles");

        c.messages = 12000; c.warmup_messages = 333; c.verify_payload = true;
        c.source.block_size = c.output.output_block_size = 4096;
        c.source.pool_min = c.source.pool_max = 2;
        c.output.output_pool_min = c.output.output_pool_max = 2;
        c.spsc_capacity = 32; c.sample_every = 1; c.max_samples = 4096;
        c.timer_calibration_samples = 1000; c.timeout_seconds = 15;
        c.call_batch_size = 31; // Deliberately does not divide the message count.
        unsigned cases = 0;
        for (const auto bytes : {8u,16u,32u,64u}) {
            auto x = c;
            x.payload_bytes = bytes; x.publish_count = UINT32_MAX; x.output.read_config.is_scan_current = false;
            x.messages = 6123; x.warmup_messages = 13; x.round_storage_bytes = bytes*2003;
            x.latency_batch_size = 1000; x.max_samples = 100; x.measure_callback_time = true;
            x.output.output_mode = TwoBufQueue::HandoffMode::SPSC;
            const perf::PublicationPlan plan(x);
            require(plan.finish_blocks && plan.warmup == 13 && plan.alignment == 1 &&
                    plan.next_end(0) == 2003 && plan.minimum_round_records() == 2003,
                    "completed-only rounds must not require a publication group");
            for (const auto transport : {perf::Transport::two_buf_queue,perf::Transport::rigtorp,
                                         perf::Transport::readerwriterqueue,perf::Transport::atomic_queue}) {
                x.transport = transport;
                for (const auto mode : {perf::MeasureMode::throughput,perf::MeasureMode::write_latency}) {
                    x.mode = mode;
                    const auto r = perf::PerformanceTest(x).run(); ++cases;
                    require(r.consumed == 6123 && r.timed_callback_messages == 6123 &&
                            r.effective_warmup_messages == 13 && r.completed_rounds == 4 &&
                            r.seconds == r.producer_seconds && r.callback_processing_seconds > 0,
                            "completed-block warmup/round drain/count/timing contract");
                    if (mode == perf::MeasureMode::write_latency)
                        require(r.latency_samples == 6 && r.latency_tail_writes == 123,
                                "completed-only rounds must retain exact latency sample accounting");
                }
            }
        }
        // Fixed reader budgets need exact full-round boundaries regardless of
        // block length or publication policy, including an unpublished tail.
        for (const auto publish : {1u,10u}) {
            for (const auto block : {1024u,4096u}) {
                auto x = c;
                x.payload_bytes = 32; x.publish_count = publish;
                x.fixed_round_boundaries = true; x.output.read_config.is_scan_current = true;
                x.source.block_size = x.output.output_block_size = block;
                x.messages = 6123; x.warmup_messages = 13; x.round_storage_bytes = 32*2003;
                x.latency_batch_size = 1000; x.max_samples = 100;
                x.measure_callback_time = true; x.output.output_mode = TwoBufQueue::HandoffMode::SPSC;
                for (const auto transport : {perf::Transport::two_buf_queue,perf::Transport::rigtorp,
                                             perf::Transport::readerwriterqueue,perf::Transport::atomic_queue}) {
                    x.transport = transport;
                    for (const auto mode : {perf::MeasureMode::throughput,perf::MeasureMode::write_latency}) {
                        x.mode = mode;
                        const perf::PublicationPlan plan(x);
                        require(plan.finish_blocks && plan.warmup == 13 && plan.alignment == 1 &&
                                plan.next_end(0) == 2003 && plan.next_end(2003) == 4006 &&
                                plan.minimum_round_records() == 2003,
                                "fixed round boundaries must not depend on source block or publish count");
                        const auto r = perf::PerformanceTest(x).run(); ++cases;
                        require(r.consumed == 6123 && r.completed_rounds == 4 &&
                                r.effective_warmup_messages == 13 && r.timed_callback_messages == 6123 &&
                                r.seconds == r.producer_seconds,
                                "fixed rounds must drain tails and keep all transports identical");
                        if (mode == perf::MeasureMode::write_latency)
                            require(r.latency_samples == 6 && r.latency_tail_writes == 123,
                                    "fixed round latency accounting");
                    }
                }
            }
        }
        // Publication count 10 must drain without active flush, including
        // partial blocks, non-multiple warmup and multiple latency rounds.
        for (const auto bytes : {8u,16u,32u,64u}) {
            auto x = c;
            x.transport = perf::Transport::two_buf_queue;
            x.payload_bytes = bytes; x.publish_count = 10;
            x.output.read_config.is_scan_current = true;
            x.output.read_config.idle_rescan_interval = std::chrono::milliseconds{1};
            x.output.read_config.idle_rescan_max_interval = std::chrono::milliseconds{1};
            x.messages = 6123; x.warmup_messages = 13;
            x.round_storage_bytes = bytes*2003; x.latency_batch_size = 1000;
            x.verify_payload = true; x.prepared_input = true; x.variable_payload = true;
            for (const auto mode : {perf::MeasureMode::throughput,perf::MeasureMode::write_latency}) {
                x.mode = mode;
                const perf::PublicationPlan plan(x);
                require(!plan.finish_blocks && plan.alignment % 10 == 0,
                        "count-10 round boundaries must be naturally published");
                const auto r = perf::PerformanceTest(x).run(); ++cases;
                require(r.consumed == x.messages && r.completed_rounds >= 4,
                        "count-10 scan-current rounds must drain without active flush");
                if (mode == perf::MeasureMode::write_latency)
                    require(r.latency_samples > 0 &&
                            r.latency_samples*x.latency_batch_size + r.latency_tail_writes == x.messages,
                            "count-10 rounds must account for every full group and round tail");
            }
        }
        for (const auto bytes : {8u,16u,32u,64u}) {
            c.payload_bytes = bytes;
            std::uint64_t expected_checksum = 0;
            for (const auto transport : {perf::Transport::two_buf_queue,perf::Transport::rigtorp,
                                         perf::Transport::readerwriterqueue,perf::Transport::atomic_queue}) {
                c.transport = transport;
                for (const auto mode : {perf::MeasureMode::throughput,perf::MeasureMode::write_latency,
                                       perf::MeasureMode::single_write_latency,perf::MeasureMode::call_batch}) {
                    c.mode = mode; c.publish_count = 32;
                    const auto r = perf::PerformanceTest(c).run(); ++cases;
                    require(r.consumed == c.messages && r.source_record_bytes == bytes,"all messages must reach the unframed per-record callback");
                    require(std::isfinite(r.seconds) && r.seconds > 0 && r.m_messages_per_second > 0,"invalid elapsed time");
                    if (!expected_checksum) expected_checksum = r.callback_work_checksum;
                    require(expected_checksum == r.callback_work_checksum,"per-message simulated work differs");
                    if (mode == perf::MeasureMode::throughput)
                        require(!r.latency_samples && r.timer_name.empty(),"throughput contains per-write timing");
                    if (mode == perf::MeasureMode::write_latency) {
                        require(r.latency_samples == c.messages/c.latency_batch_size,"one sample per full group");
                        require(r.batch_average_write_ns.p50 <= r.batch_average_write_ns.p99 &&
                                r.batch_average_write_ns.p99 <= r.batch_average_write_ns.p999,"group percentile order");
                        require(r.latency_quantum_ns == r.timer_quantum_ns/c.latency_batch_size,"scaled clock quantum");
                        std::ostringstream json; perf::write_result(json,r);
                        require(json.str().find("\"successful_write_ns\":null") != std::string::npos &&
                                json.str().find("\"latency_kind\":\"batch_average\"") != std::string::npos,
                                "do not label group averages as individual calls");
                    }
                    if (mode == perf::MeasureMode::single_write_latency) {
                        require(r.latency_samples > 0 && r.latency_samples <= c.max_samples,"sample cap");
                        require(r.successful_write_ns.p50 <= r.successful_write_ns.p99 &&
                                r.successful_write_ns.p99 <= r.successful_write_ns.p999,"percentile order");
                    }
                    if (mode == perf::MeasureMode::call_batch)
                        require((r.call_batch_attempts >= c.messages || r.migrated_samples) && r.call_batch_mean_ns >= 0 && !r.latency_samples,
                                "batch mean must not be reported as single-call percentiles");
                }
            }
        }
        for (const auto transport : {perf::Transport::two_buf_queue,perf::Transport::rigtorp,
                                     perf::Transport::readerwriterqueue,perf::Transport::atomic_queue}) {
            c.transport = transport; c.mode = perf::MeasureMode::write_latency;
            c.messages = 12345; c.latency_batch_size = 37; c.max_samples = 7;
            const auto r = perf::PerformanceTest(c).run(); ++cases;
            require(r.consumed == 12345 && r.latency_samples == 7 && r.latency_tail_writes == 12345%37,
                    "custom group size, sample cap and final partial group");
        }
        {
            auto legacy=c;
            legacy.messages=10000; legacy.warmup_messages=128;
            legacy.round_storage_bytes=0; legacy.measure_callback_time=false;
            legacy.prepared_input=false; legacy.variable_payload=false;
            legacy.verify_payload=true; legacy.mode=perf::MeasureMode::throughput;
            const auto r=perf::PerformanceTest(legacy).run(); ++cases;
            require(r.consumed==legacy.messages && !r.prepared_input_bytes &&
                    !r.input_preparation_seconds,"legacy diagnostic input mode");
            auto corrupt=c;
            corrupt.messages=2; corrupt.warmup_messages=1; corrupt.round_storage_bytes=0;
            corrupt.verify_payload=true; corrupt.variable_payload=true;
            const perf::PublicationPlan plan(corrupt);
            perf::detail::Sink<32> sink{corrupt,plan,0,2};
            perf::Payload<32> value;
            value.bytes.fill(std::byte{0xa5});
            perf::detail::set_payload(value,0,true); sink(0,value.bytes.data(),32);
            require(sink.valid,"valid variable payload rejected");
            perf::detail::set_payload(value,1,true); value.bytes[31]^=std::byte{1};
            sink(0,value.bytes.data(),32);
            require(!sink.valid,"tail-byte corruption was not detected");
        }
        for (const auto publish : {1u,2u,4u,8u,16u,32u,64u,128u,256u,512u,1024u}) {
            c.source.block_size = c.output.output_block_size = 65536;
            c.transport = perf::Transport::two_buf_queue; c.publish_count = publish;
            c.mode = perf::MeasureMode::throughput; c.callback_iterations = 0;
            c.warmup_messages = 0; // A naturally published group starts the consumer.
            const auto r = perf::PerformanceTest(c).run(); ++cases;
            require(r.consumed == c.messages && r.callback_work_checksum == 0,"zero simulated work / tail finish");
        }
        c.publish_count = 32;
        // Publication restarts at each source block; global count alignment is insufficient.
        for (const auto bytes : {8u,16u,32u,64u}) {
            auto x = c; x.payload_bytes = bytes; x.messages = 6123;
            x.warmup_messages = 400; x.round_storage_bytes = bytes*2003;
            x.mode = perf::MeasureMode::write_latency; x.latency_batch_size = 1000;
            const perf::PublicationPlan plan(x);
            require(plan.warmup >= x.warmup_messages && plan.warmup-x.warmup_messages <= 2*(plan.alignment-1),
                    "warmup rounding must be bounded");
            const auto position = [&](std::uint64_t count) { return (count-1)%plan.records_per_block+1; };
            require(position(plan.warmup)%plan.alignment == 0,"warmup must end at a natural publication");
            for (std::uint64_t sent=0; sent<x.messages;) {
                const auto end = plan.next_end(sent);
                require(end>sent && end-sent<=plan.target_records && end<=x.messages,"round progress and budget");
                if (end<x.messages) {
                    require(end-sent>=plan.minimum_round_records(),"bounded round adjustment");
                    for (std::uint32_t publish=1; publish<=plan.alignment; publish*=2)
                        require(position(plan.warmup+end)%publish == 0,"boundary naturally published for every candidate");
                }
                sent=end;
            }
        }
        {
            auto x = c; x.messages = 1; x.warmup_messages = 0; x.round_storage_bytes = 0;
            const perf::PublicationPlan plan(x);
            require(plan.next_end(0)==1 && plan.minimum_round_records()==1,"one-message final tail must not underflow");
            x.payload_bytes = 8; x.source.block_size = 64; x.publish_count = 32;
            invalid([&] { (void)perf::PublicationPlan(x); });
        }
        std::ostringstream defaults; perf::write_config(defaults,c);
        c.round_storage_bytes = c.payload_bytes*2003;
        c.latency_batch_size = 1000; c.max_samples = 100;
        c.messages = 6123; c.callback_iterations = 7; c.prefault_producer_buffers = true;
        c.measure_callback_time = true;
        c.output.output_mode = TwoBufQueue::HandoffMode::SPSC;
        invalid([&] { auto x = c; x.output.output_mode = TwoBufQueue::HandoffMode::Sync; x.validate(); });
        for (const auto transport : {perf::Transport::two_buf_queue,perf::Transport::rigtorp,
                                     perf::Transport::readerwriterqueue,perf::Transport::atomic_queue}) {
            c.transport = transport;
            for (const auto mode : {perf::MeasureMode::throughput,perf::MeasureMode::write_latency}) {
                c.mode = mode;
#if defined(__APPLE__)
                c.producer_qos = perf::ProducerQos::user_interactive;
                qos_class_t before_qos; int before_priority;
                require(!pthread_get_qos_class_np(pthread_self(),&before_qos,&before_priority),"read caller QoS");
#endif
                const auto r = perf::PerformanceTest(c).run(); ++cases;
                require(r.consumed == 6123 && r.completed_rounds == 4,"partial final round / exact callback completion");
                require(r.seconds == r.producer_seconds && r.wall_seconds >= r.seconds &&
                        r.drain_seconds > 0,"producer time excludes round drains");
                require(r.timed_callback_messages == c.messages && r.callback_processing_seconds > 0 &&
                        r.callback_to_producer_ratio == r.callback_processing_seconds/r.producer_seconds,
                        "callback timing excludes warmup and counts every formal callback");
                require(r.backpressured_messages<=r.failed_write_calls &&
                        r.producer_retry_max_seconds<=r.producer_retry_seconds,"formal retry accounting");
                require(r.library_pressure_stats_available==(transport==perf::Transport::two_buf_queue),
                        "native queue internal stats availability");
                if (!r.failed_write_calls) require(!r.producer_retry_seconds && !r.backpressured_messages,
                        "success-only sends must not run retry clocks");
                std::ostringstream timing_json; perf::write_result(timing_json,r);
                require(timing_json.str().find("round_first_to_last_callback_including_dispatch") != std::string::npos,
                        "callback timing must describe its actual scope");
                if (mode == perf::MeasureMode::write_latency)
                    require(r.latency_samples == 3 && r.latency_tail_writes == 3123,
                            "latency groups cannot span a round drain");
#if defined(__APPLE__)
                require(r.producer_qos_applied && !r.producer_hard_affinity,"high QoS is not CPU affinity");
                qos_class_t after_qos; int after_priority;
                require(!pthread_get_qos_class_np(pthread_self(),&after_qos,&after_priority),"read restored QoS");
                require(after_qos == (before_qos == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_DEFAULT : before_qos) &&
                        after_priority == before_priority,"do not leave high QoS on the caller after run");
#endif
            }
        }
        // 控制线程只能在整轮最后一条的业务循环执行完后观察到完成。
        {
            perf::PerformanceConfig x; x.messages = 5; x.warmup_messages = 0; x.round_storage_bytes = 24;
            x.measure_callback_time = true;
            const perf::PublicationPlan plan(x);
            require(plan.warmup == 2 && plan.alignment == 2,"small rounds select a fitting publication group");
            perf::detail::Sink<8> sink{x,plan,2,7};
            perf::Payload<8> value{};
            for (std::uint64_t i=0; i<7; ++i) {
                std::memcpy(value.bytes.data(),&i,sizeof(i)); sink(0,value.bytes.data(),8);
                require(sink.completed_rounds.load() == (i < 3 ? 0u : i < 6 ? 1u : 2u),
                        "round completion published before last callback");
                require(sink.timed_callback_messages == (i < 3 ? 0u : i < 6 ? 2u : 5u),
                        "callback timing boundary and final partial round");
            }
        }
        // Pacing is selected once; the continuous specialization has no pace branch.
        for (const auto transport : {perf::Transport::two_buf_queue,perf::Transport::readerwriterqueue,
                                     perf::Transport::rigtorp,perf::Transport::atomic_queue}) {
            for (const auto mode : {perf::MeasureMode::throughput,perf::MeasureMode::single_write_latency}) {
                perf::PerformanceConfig paced; paced.transport=transport; paced.mode=mode;
                paced.messages=1000; paced.warmup_messages=64; paced.publish_count=4;
                paced.producer_interval_ns=1000; paced.verify_payload=true;
                paced.output.output_mode=TwoBufQueue::HandoffMode::SPSC;
                const auto r=perf::PerformanceTest(paced).run(); ++cases;
                require(r.consumed==paced.messages && r.producer_seconds>=0.0009,
                        "paced specialization must preserve message count and interval");
            }
        }
        invalid([] { perf::PerformanceConfig x; perf::set_parameter(x,"cache_remaining_bytes","true"); });
        for (const auto bytes : {8u,16u,32u,64u}) {
            for (const auto mode : {perf::MeasureMode::throughput,perf::MeasureMode::write_latency}) {
                perf::PerformanceConfig x; x.payload_bytes=bytes; x.messages=10123; x.warmup_messages=65;
                x.mode=mode; x.publish_count=UINT32_MAX;
                x.output.read_config.is_scan_current=false; x.output.output_mode=TwoBufQueue::HandoffMode::SPSC;
                x.source.block_size=1024; x.source.pool_min=x.source.pool_max=128;
                x.output.output_block_size=2048; x.output.output_pool_min=x.output.output_pool_max=64;
                x.round_storage_bytes=65536; x.prepared_input=x.variable_payload=x.verify_payload=true;
                const auto r=perf::PerformanceTest(x).run(); ++cases;
                require(r.consumed==x.messages && !r.failed_write_calls,"remaining-capacity full-data/round check");
                std::ostringstream serialized; perf::write_result(serialized,r);
                require(serialized.str().find("cache_remaining_bytes")==std::string::npos,
                        "removed capacity switch still present in metadata");
            }
        }
        invalid([] { perf::PerformanceConfig x; x.use_write_session=true; x.validate(); });
        invalid([] { perf::PerformanceConfig x; x.use_write_session=true;
            x.publish_count=UINT32_MAX; x.mode=perf::MeasureMode::single_write_latency; x.validate(); });
        invalid([] { perf::PerformanceConfig x; x.use_write_session=true;
            x.publish_count=UINT32_MAX; x.prepared_input=false; x.validate(); });
        for (const auto bytes : {8u,16u,32u,64u}) {
            for (const auto mode : {perf::MeasureMode::throughput,perf::MeasureMode::write_latency}) {
                perf::PerformanceConfig x; x.payload_bytes=bytes; x.messages=10123; x.warmup_messages=65;
                x.mode=mode; x.publish_count=UINT32_MAX; x.use_write_session=true;
                x.output.read_config.is_scan_current=false; x.output.output_mode=TwoBufQueue::HandoffMode::SPSC;
                x.source.block_size=1024; x.source.pool_min=x.source.pool_max=128;
                x.output.output_block_size=2048; x.output.output_pool_min=x.output.output_pool_max=64;
                x.round_storage_bytes=65536; x.prepared_input=x.variable_payload=x.verify_payload=true;
                const auto r=perf::PerformanceTest(x).run(); ++cases;
                require(r.consumed==x.messages && !r.failed_write_calls && r.session_block_exhaustions > 0,
                        "write-session full-data/round/block-boundary check");
                std::ostringstream serialized; perf::write_result(serialized,r);
                require(serialized.str().find("\"use_write_session\":true")!=std::string::npos,
                        "write-session reproduction metadata missing");
            }
        }
        require(defaults.str().find("\"callback_iterations\":0") != std::string::npos,"reproduction metadata missing");
        std::cout << cases << " performance harness contract cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
