#pragma once
#include <array>
#include <cstdint>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <algorithm>
#include <stdexcept>
#include <vector>
namespace instruction_probe {
struct Counts { std::uint64_t instructions=0,cycles=0,user=0,system=0; };
struct Sample {
    std::uint64_t instructions=0,cycles=0;
    double user_ns=0,system_ns=0;
    std::size_t cpu_begin=0,cpu_end=0;
};
class Probe {
    using Read=int(*)(std::uint32_t,void*,std::size_t);
    Read read_;
    double ns_;
public:
    std::array<double,3> overhead_ns{},empty_instructions{},empty_cycles{};
    Probe(){
        read_=reinterpret_cast<Read>(dlsym(RTLD_DEFAULT,"thread_selfcounts"));
        if(!read_)throw std::runtime_error("thread_selfcounts unavailable");
        mach_timebase_info_data_t tb{};if(mach_timebase_info(&tb))throw std::runtime_error("timebase unavailable");
        ns_=double(tb.numer)/tb.denom;
        std::vector<double> cost,ins,cyc;cost.reserve(10000);ins.reserve(10000);cyc.reserve(10000);
        for(int i=0;i<10000;++i){Counts a,b;auto t=mach_absolute_time();read(a);auto u=mach_absolute_time();read(b);cost.push_back((u-t)*ns_);ins.push_back(b.instructions-a.instructions);cyc.push_back(b.cycles-a.cycles);}
        for(auto* v:{&cost,&ins,&cyc})std::sort(v->begin(),v->end());
        for(int k=0;k<3;++k){const int index=k==0?4999:k==1?9899:9989;overhead_ns[k]=cost[index];empty_instructions[k]=ins[index];empty_cycles[k]=cyc[index];}
    }
    void read(Counts& counts) const {if(read_(3,&counts,sizeof(counts)))throw std::runtime_error("cannot read per-thread fixed counters");}
    std::size_t cpu() const {std::size_t value=0;if(pthread_cpu_number_np(&value))throw std::runtime_error("CPU query failed");return value;}
    Sample difference(Counts a,Counts b,std::size_t ca,std::size_t cb) const {
        if(b.instructions<a.instructions || b.cycles<a.cycles || b.user<a.user || b.system<a.system)throw std::runtime_error("hardware counters reversed");
        return {b.instructions-a.instructions,b.cycles-a.cycles,(b.user-a.user)*ns_,(b.system-a.system)*ns_,ca,cb};
    }
};
}
