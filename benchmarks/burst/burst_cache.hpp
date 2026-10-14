#pragma once
#include "../hot_path/hot_path_cache.hpp"
#include <sstream>
#include <cstdio>
void burst_prefault_aligned_buffers(bool value) noexcept;
namespace TwoBufQueue::performance::burst {
inline constexpr std::uint64_t setup_tag = UINT64_C(1) << 63;
struct Config {
    PerformanceConfig queue;
    bool baseline = false, prefill = true;
    bool drain_before_burst = false; // Wait outside timing so each burst starts with zero pending records.
    std::size_t source_pool_bytes = 32*1024;
    std::size_t business_bytes = 48*1024;
    std::uint32_t accesses = 8, warm_queries = 512, query_group = 16, query_groups = 4;
    std::uint32_t repeats = 3;
    std::uint32_t target_percent = 80, setup_timeout_ms = 3000;
    std::uint32_t progress_chunk = 1024;
    std::uint32_t progress_every = 0; // Control-path progress; never inside timed sections.
    std::vector<std::size_t> bursts;
    Config() {
        hot_path::Config c; queue = c.queue;
        queue.source.pool_min = queue.source.pool_max = 4;
        queue.prefault_producer_buffers = true;
        queue.callback_iterations = 64;
        queue.payload_bytes = 32; queue.timeout_seconds = 180;
        for (std::size_t b=16*1024; b<=128*hot_path::MiB; b*=2) bursts.push_back(b);
    }
    void prepare() {
        if (prefill && drain_before_burst) throw std::invalid_argument("prefill and drain_before_burst cannot both be enabled");
        const auto out_bytes = queue.spsc_buffer_bytes;
        if (source_pool_bytes < 128 || source_pool_bytes % 128 || !queue.source.pool_max)
            throw std::invalid_argument("invalid total writer pool bytes");
        queue.source.block_size = source_pool_bytes / queue.source.pool_max;
        queue.source.pool_min = queue.source.pool_max;
        queue.output.output_block_size = queue.source.block_size;
        if (out_bytes % queue.output.output_block_size) throw std::invalid_argument("output budget must divide by block size");
        queue.output.output_pool_min = queue.output.output_pool_max = out_bytes/queue.output.output_block_size;
        queue.output.output_queue_capacity = queue.output.output_pool_max+2;
        queue.output.output_frame_capacity = 4;
        queue.round_storage_bytes = out_bytes/100*target_percent;
        queue.validate();
        if (queue.payload_bytes != 32 || !queue.callback_iterations || queue.publish_count != UINT32_MAX ||
            queue.output.read_config.is_scan_current || queue.use_write_session || queue.prepared_input ||
            queue.source.block_size < 32 || queue.source.block_size % 32 || target_percent < 50 || target_percent>80 ||
            !setup_timeout_ms || !progress_chunk || !repeats || !query_group || !query_groups || !accesses ||
            business_bytes<64 || business_bytes%64 || bursts.empty()) throw std::invalid_argument("invalid burst configuration");
        const auto limit = out_bytes/100*target_percent;
        for (auto b : bursts) if (b<32 || b%32 || b>=limit) throw std::invalid_argument("burst must fit below target backlog bytes");
    }
};
struct Sink {
    const std::uint32_t loops;
    alignas(128) std::atomic<std::uint64_t> completed{0};
    std::uint64_t setup_count=0, formal_count=0, work=0;
    bool valid=true;
    explicit Sink(std::uint32_t count) : loops(count) {}
    void operator()(std::int32_t id,const std::byte* p,std::size_t size) noexcept {
        std::array<std::uint64_t,4> w{};
        if (size==32) std::memcpy(w.data(),p,32); else valid=false;
        valid &= id==0;
        const bool is_setup = (w[0]&setup_tag)!=0;
        const auto index=w[0]&~setup_tag;
        valid &= index==(is_setup ? setup_count : formal_count);
        for (std::size_t i=1; i<4; ++i) valid &= w[i]==detail::payload_word(w[0],i);
        work=hot_path::callback_work(work^w[0],loops);
        if (is_setup) ++setup_count; else ++formal_count;
        // Writer reads this only in control paths, before/after a burst.
        completed.store(setup_count+formal_count,std::memory_order_release);
    }
};
inline Payload<32> payload(std::uint64_t seq) noexcept {
    Payload<32> p; detail::set_payload(p,seq,true); return p;
}
class NoQueue {
public:
    NoQueue(const PerformanceConfig&,Sink&) {}
    bool push(const Payload<32>& p) noexcept {
        std::uint64_t v; std::memcpy(&v,p.bytes.data(),8); detail::observable(v); return true;
    }
    void finish_block() noexcept {} void finish() noexcept {} void stop() noexcept {} void check() const {}
    std::size_t capacity() const noexcept { return 0; }
    PressureSnapshot pressure_stats() const noexcept { return {}; }
};
struct Point {
    std::size_t burst_bytes=0;
    std::uint32_t repetition=0;
    double writer_ns=0, writer_m_messages_s=0, pre_business_ns=0;
    std::vector<double> after_business_ns;
    std::uint64_t pending_before=0,pending_after=0,output_pending_after=0;
    std::uint64_t retries=0,backpressured_messages=0,setup_messages=0;
    double control_seconds=0;
    bool setup_target_reached=false;
};
struct Result {
    Config config;
    std::vector<Point> points;
    std::uint64_t accepted=0,formal=0,setup=0,completed=0,business_checksum=0,callback_checksum=0;
    std::size_t actual_native_capacity=0;
    double elapsed_seconds=0,rss_mib=0,timer_quantum_ns=0;
    std::string timer_name;
    bool qos_applied=false;
};
template<class Channel> Result run_with(const Config& c) {
    Result r; r.config=c;
    detail::Watchdog watchdog(c.queue.timeout_seconds);
    Sink sink(c.queue.callback_iterations);
    burst_prefault_aligned_buffers(true);
    struct PrefaultGuard { ~PrefaultGuard(){burst_prefault_aligned_buffers(false);} } prefault_guard;
    Channel channel(c.queue,sink);
    burst_prefault_aligned_buffers(false);
    detail::ProducerAffinity affinity(c.queue.producer_cpu);
    PerformanceResult schedule; detail::ProducerScheduling scheduling(c.queue.producer_qos,schedule);
    r.qos_applied=schedule.producer_qos_applied; r.actual_native_capacity=channel.capacity();
    hot_path::Config business_config; business_config.business_bytes=c.business_bytes;
    business_config.business_accesses=c.accesses; business_config.queue.random_seed=c.queue.random_seed;
    hot_path::Business business(business_config);
    ProbeClock clock(c.queue); r.timer_name=clock.label();r.timer_quantum_ns=clock.quantum_ns();
    r.points.reserve(c.bursts.size()*c.repeats);
    const auto start=std::chrono::steady_clock::now();
    std::uint64_t accepted=0,setup=0,formal=0;
    const auto pending=[&]() -> std::uint64_t {
        if (c.baseline) return 0;
        const auto done=sink.completed.load(std::memory_order_acquire);
        return (accepted>=done ? accepted-done : 0)*32;
    };
    // Only used for preparing occupancy, never pauses callback execution.
    const auto adjust=[&](std::uint64_t target,Point& p) {
        if (c.baseline || !c.prefill) return;
        const auto control_start=std::chrono::steady_clock::now();
        const auto deadline=control_start+std::chrono::milliseconds(c.setup_timeout_ms);
        while (std::chrono::steady_clock::now()<deadline) {
            channel.check();
            const auto bytes=pending();
            if (bytes>=target && bytes<=target+std::uint64_t(c.progress_chunk)*32) { p.setup_target_reached=true;break; }
            if (bytes>target) { std::this_thread::sleep_for(std::chrono::microseconds(50)); continue; }
            const auto count=std::min<std::uint64_t>(c.progress_chunk,(target-bytes+31)/32);
            for (std::uint64_t i=0;i<count;++i) {
                const auto value=payload(setup_tag|setup);
                while (!channel.push(value)) {
                    channel.check(); std::this_thread::yield();
                    if (std::chrono::steady_clock::now()>=deadline) {
                        channel.finish_block(); p.control_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-control_start).count(); return;
                    }
                }
                ++accepted;++setup;++p.setup_messages;
            }
            channel.finish_block();
        }
        p.control_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-control_start).count();
    };
    const auto queries=[&](std::uint32_t n) {
        for (std::uint32_t i=0;i<n;++i) r.business_checksum+=business.step();
    };
    const auto timed_queries=[&](std::uint32_t n) {
        const auto a=clock.read(); queries(n);const auto b=clock.read();return clock.elapsed(a,b)/n;
    };
    const auto peak_target=c.queue.spsc_buffer_bytes/100*c.target_percent;
    // Ascending bursts share one continuously running consumer and one queue.
    // Occupancy adjustments and final drain stay outside measured sections.
    for (auto bytes:c.bursts) for (std::uint32_t rep=1;rep<=c.repeats;++rep) {
        Point p;p.burst_bytes=bytes;p.repetition=rep;p.after_business_ns.reserve(c.query_groups);
        if (c.drain_before_burst && !c.baseline) {
            const auto drain_start = std::chrono::steady_clock::now();
            channel.finish_block();
            const auto deadline = drain_start + std::chrono::milliseconds(c.setup_timeout_ms);
            while (sink.completed.load(std::memory_order_acquire) < accepted) {
                channel.check();
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("empty-before-burst drain timeout");
                std::this_thread::yield();
            }
            p.control_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now()-drain_start).count();
        }
        adjust(peak_target-bytes,p);
        queries(c.warm_queries);
        p.pre_business_ns=timed_queries(c.query_group);
        p.pending_before=pending();
        if (c.drain_before_burst && p.pending_before != 0)
            throw std::runtime_error("burst did not start empty");
        const auto a=clock.read();
        for (std::uint64_t i=0;i<bytes/32;++i) {
            const auto value=payload(formal);
            if (!channel.push(value)) {
                ++p.backpressured_messages;
                do { ++p.retries; cpu_relax(); if(c.queue.retry_yield_every && p.retries%c.queue.retry_yield_every==0)std::this_thread::yield(); } while(!channel.push(value));
            }
            ++formal;
        }
        const auto b=clock.read();
        p.writer_ns=clock.elapsed(a,b);p.writer_m_messages_s=double(bytes/32)/p.writer_ns*1000;
        accepted+=bytes/32;
        // Measure the business immediately, before publication/control instrumentation.
        for (std::uint32_t group=0;group<c.query_groups;++group) p.after_business_ns.push_back(timed_queries(c.query_group));
        p.pending_after=pending();
        const auto stats=channel.pressure_stats();
        if(stats.available) {
            const auto cb=sink.completed.load(std::memory_order_acquire)*32;
            p.output_pending_after=stats.copy.saved_data_bytes>cb ? stats.copy.saved_data_bytes-cb : 0;
        }
        channel.finish_block();
        r.points.push_back(std::move(p));
        if(c.progress_every && rep%c.progress_every==0)
            std::fprintf(stderr,"burst_progress bytes=%zu window=%u/%u\n",bytes,rep,c.repeats);
    }
    channel.finish_block();
    if(!c.baseline) while(sink.completed.load(std::memory_order_acquire)<accepted){channel.check();std::this_thread::sleep_for(std::chrono::microseconds(100));}
    channel.finish();channel.stop();channel.check();
    r.accepted=accepted;r.formal=formal;r.setup=setup;r.completed=c.baseline?accepted:sink.completed.load();
    r.callback_checksum=sink.work;r.rss_mib=benchmark::peak_rss_mib();r.elapsed_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    if(!c.baseline && (!sink.valid||r.completed!=accepted||sink.formal_count!=formal||sink.setup_count!=setup))throw std::runtime_error("burst full payload/FIFO/count validation failed");
    return r;
}
inline Result run(Config c) {
    c.prepare();if(c.baseline)return run_with<NoQueue>(c);
    using P=Payload<32>;
    switch(c.queue.transport){
    case Transport::two_buf_queue:return run_with<TwoBufChannel<UINT32_MAX,Sink,32>>(c);
    case Transport::readerwriterqueue:
        if(c.queue.readerwriterqueue_copy_after_zero)return run_with<SpscChannel<Sink,ReaderWriterAdapter<P,true>>>(c);
        return run_with<SpscChannel<Sink,ReaderWriterAdapter<P,false>>>(c);
    case Transport::rigtorp:
        if(c.queue.rigtorp_write_prefetch)return run_with<SpscChannel<Sink,RigtorpAdapter<P,true>>>(c);
        return run_with<SpscChannel<Sink,RigtorpAdapter<P,false>>>(c);
    case Transport::atomic_queue:return run_with<SpscChannel<Sink,AtomicAdapter<P>>>(c);
    }throw std::invalid_argument("transport unavailable");
}
inline void write_result(std::ostream& o,const Result& r){
    o<<std::setprecision(12)<<"{\"schema\":\"burst_cache_v1\",\"transport\":\""<<(r.config.baseline?"baseline":name(r.config.queue.transport))<<"\",\"queue_config\":";write_config(o,r.config.queue);
    o<<",\"source_pool_bytes\":"<<r.config.source_pool_bytes<<",\"business_bytes\":"<<r.config.business_bytes<<",\"business_accesses\":"<<r.config.accesses<<",\"query_group\":"<<r.config.query_group<<",\"query_groups\":"<<r.config.query_groups<<",\"warm_queries\":"<<r.config.warm_queries<<",\"prefill\":"<<(r.config.prefill?"true":"false")<<",\"drain_before_burst\":"<<(r.config.drain_before_burst?"true":"false")<<",\"setup_timeout_ms\":"<<r.config.setup_timeout_ms<<",\"target_percent\":"<<r.config.target_percent<<",\"progress_chunk\":"<<r.config.progress_chunk<<",\"repeats\":"<<r.config.repeats<<",\"progress_every\":"<<r.config.progress_every;
#define FIELD(x) o<<",\"" #x "\":"<<r.x
    FIELD(accepted);FIELD(formal);FIELD(setup);FIELD(completed);FIELD(business_checksum);FIELD(callback_checksum);FIELD(actual_native_capacity);FIELD(elapsed_seconds);FIELD(rss_mib);FIELD(timer_quantum_ns);
#undef FIELD
    o<<",\"qos_applied\":"<<(r.qos_applied?"true":"false")<<",\"hard_affinity\":"<<(r.config.queue.producer_cpu>=0?"true":"false")<<",\"timer_name\":\""<<r.timer_name<<"\",\"verified\":true,\"points\":[";
    bool first=true;for(const auto&p:r.points){if(!first)o<<',';first=false;o<<'{';
#define POINT(x) o<<"\"" #x "\":"<<p.x<<','
        POINT(burst_bytes);POINT(repetition);POINT(writer_ns);POINT(writer_m_messages_s);POINT(pre_business_ns);POINT(pending_before);POINT(pending_after);POINT(output_pending_after);POINT(retries);POINT(backpressured_messages);POINT(setup_messages);POINT(control_seconds);
#undef POINT
        o<<"\"setup_target_reached\":"<<(p.setup_target_reached?"true":"false")<<",\"after_business_ns\":[";bool a=true;for(auto ns:p.after_business_ns){if(!a)o<<',';a=false;o<<ns;}o<<"]}";
    }o<<"]}";
}
}
