#pragma once
#include <cstdint>
#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <mach/mach_time.h>
extern "C"{int brq_pmu_open(const char*,int);void brq_pmu_close();int brq_pmu_read(std::uint64_t*);int brq_pmu_count();const char*brq_pmu_name(int);}
namespace instruction_probe {
class Pmu {
    bool enabled_=false;
public:
    std::string group;
    std::array<double,3> overhead{};
    std::array<std::uint64_t,8> empty{};
    Pmu(){
        const char*value=std::getenv("BRQ_PMU_GROUP");if(!value)return;group=value;
        if(brq_pmu_open(value,0))throw std::runtime_error("cannot configure PMU");enabled_=true;
        mach_timebase_info_data_t tb;mach_timebase_info(&tb);double ns=double(tb.numer)/tb.denom;
        std::vector<double> cost;std::array<std::vector<std::uint64_t>,8> deltas;
        cost.reserve(10000);for(auto&d:deltas)d.reserve(10000);
        for(int i=0;i<10000;++i){std::array<std::uint64_t,8>a{},b{};auto t=mach_absolute_time();read(a);auto u=mach_absolute_time();read(b);cost.push_back((u-t)*ns);for(int j=0;j<8;++j)deltas[j].push_back(b[j]-a[j]);}
        std::sort(cost.begin(),cost.end());for(int k=0;k<3;++k)overhead[k]=cost[k==0?4999:k==1?9899:9989];
        for(int j=0;j<8;++j){std::sort(deltas[j].begin(),deltas[j].end());empty[j]=deltas[j][4999];}
    }
    bool enabled()const noexcept{return enabled_;}
    void read(std::array<std::uint64_t,8>&counts){if(!enabled_||brq_pmu_read(counts.data()))throw std::runtime_error("PMU read failed");}
    ~Pmu(){if(enabled_)brq_pmu_close();}
    Pmu(const Pmu&)=delete;Pmu&operator=(const Pmu&)=delete;
};
}
