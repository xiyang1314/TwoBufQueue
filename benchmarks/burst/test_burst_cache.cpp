#include "burst_cache.hpp"
#include <iostream>
using namespace TwoBufQueue::performance;
namespace bc=TwoBufQueue::performance::burst;
void self_test(){
    bc::Config c;c.queue.producer_qos=ProducerQos::unchanged;c.queue.spsc_buffer_bytes=1<<20;c.queue.spsc_capacity=32768;c.source_pool_bytes=2048;c.business_bytes=8192;c.bursts={16*1024,64*1024};c.repeats=2;c.queue.callback_iterations=4;c.setup_timeout_ms=300;c.target_percent=60;c.warm_queries=32;
    std::uint64_t business=0;
    for(auto t:{Transport::two_buf_queue,Transport::readerwriterqueue,Transport::rigtorp,Transport::atomic_queue}){
        c.queue.transport=t;auto r=bc::run(c);if(r.points.size()!=4||r.formal!=2*(16+64)*1024/32||r.accepted!=r.completed)throw std::runtime_error("burst counts");
        if(business&&business!=r.business_checksum)throw std::runtime_error("different business workload");
        business=r.business_checksum;
        if(r.config.queue.source.block_size!=512)throw std::runtime_error("2KiB means total pool");
    }
    c.prefill=false;c.drain_before_burst=true;
    for(auto t:{Transport::two_buf_queue,Transport::readerwriterqueue,Transport::rigtorp,Transport::atomic_queue}){
        c.queue.transport=t;auto empty=bc::run(c);
        if(empty.setup || empty.accepted!=empty.formal || empty.accepted!=empty.completed || empty.business_checksum!=business)
            throw std::runtime_error("empty-burst counts or workload");
        for(const auto& point:empty.points)
            if(point.pending_before || point.pending_after>point.burst_bytes || point.setup_messages)
                throw std::runtime_error("empty-burst backlog invariant");
    }
    c.baseline=true;auto r=bc::run(c);if(r.business_checksum!=business)throw std::runtime_error("baseline differs");
    std::cout<<"burst contracts passed: 2KiB total, live slow callbacks, full payloads, FIFO, matching business, all queues\n";
}
int main(int argc,char**argv){try{
    bc::Config c;
    for(int i=1;i<argc;++i){std::string_view arg(argv[i]);if(arg=="--self-test"){self_test();return 0;}if(!arg.starts_with("--")||i+1==argc)throw std::invalid_argument("expected --key value");auto k=arg.substr(2);std::string_view v(argv[++i]);
        if(k=="transport"&&v=="baseline")c.baseline=true;
        else if(k=="source_pool_bytes")assign_parameter(c.source_pool_bytes,v);
        else if(k=="business_bytes")assign_parameter(c.business_bytes,v);
        else if(k=="business_accesses")assign_parameter(c.accesses,v);
        else if(k=="warm_queries")assign_parameter(c.warm_queries,v);
        else if(k=="query_group")assign_parameter(c.query_group,v);
        else if(k=="query_groups")assign_parameter(c.query_groups,v);
        else if(k=="repeats")assign_parameter(c.repeats,v);
        else if(k=="progress_every")assign_parameter(c.progress_every,v);
        else if(k=="target_percent")assign_parameter(c.target_percent,v);
        else if(k=="setup_timeout_ms")assign_parameter(c.setup_timeout_ms,v);
        else if(k=="prefill")assign_parameter(c.prefill,v);
        else if(k=="drain_before_burst")assign_parameter(c.drain_before_burst,v);
        else if(k=="burst_bytes"){std::size_t b;assign_parameter(b,v);c.bursts={b};}
        else if(k=="burst_sizes"){
            c.bursts.clear();
            while(!v.empty()){
                const auto comma=v.find(',');
                std::size_t size=0;assign_parameter(size,v.substr(0,comma));c.bursts.push_back(size);
                if(comma==std::string_view::npos)break;
                v.remove_prefix(comma+1);
                if(v.empty())throw std::invalid_argument("empty burst size");
            }
        }
        else set_parameter(c.queue,k,v);
    }
    bc::write_result(std::cout,bc::run(c));std::cout<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<"burst cache: "<<e.what()<<'\n';return 1;}}
