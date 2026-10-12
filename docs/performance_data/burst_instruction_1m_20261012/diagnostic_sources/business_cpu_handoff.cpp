#include "hot_path_cache.hpp"
#include "fixed_probe.hpp"
#include <atomic>
#include <thread>
#include <iostream>
#include <iomanip>
#include <pthread.h>
using namespace TwoBufQueue::performance;
struct Record {std::size_t before_cpu=0,after_cpu=0;std::array<double,4> group_ns{};instruction_probe::Sample counters{};};
int main(int argc,char**argv){
 const unsigned repeats=argc>1?std::stoul(argv[1]):10000;
 const bool counters=argc>2?std::stoul(argv[2])!=0:false;
 hot_path::Config config;config.business_bytes=49152;config.business_accesses=8;config.queue.random_seed=42;
 hot_path::Business business(config);
 PerformanceConfig timer_config;ProbeClock clock(timer_config);
 std::atomic<int> task{0};std::uint64_t checksum=0;Record remote_record;
 std::vector<Record> local,remote;local.reserve(repeats);remote.reserve(repeats);
 pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
 instruction_probe::Probe main_probe;
 const auto measure=[&](instruction_probe::Probe&probe){
  Record r;r.before_cpu=probe.cpu();instruction_probe::Counts a,b;if(counters)probe.read(a);
  for(int j=0;j<4;++j){auto start=clock.read();for(int i=0;i<16;++i)checksum+=business.step();auto stop=clock.read();r.group_ns[j]=clock.elapsed(start,stop)/16;}
  if(counters)probe.read(b);r.after_cpu=probe.cpu();if(counters)r.counters=probe.difference(a,b,r.before_cpu,r.after_cpu);return r;
 };
 std::thread worker([&]{pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);instruction_probe::Probe probe;
  for(;;){int state;do{state=task.load(std::memory_order_acquire);TwoBufQueue::cpu_relax();}while(!state);if(state==2)break;remote_record=measure(probe);task.store(0,std::memory_order_release);}
 });
 std::cout<<"{\"repeats\":"<<repeats<<",\"counters\":"<<(counters?"true":"false")<<",\"records\":[";
 for(unsigned k=0;k<repeats;++k){
  // Every business line is read and modified on the main CPU before each arm.
  for(int i=0;i<512;++i)checksum+=business.step();auto warm_local_cpu=main_probe.cpu();auto a=measure(main_probe);
  for(int i=0;i<512;++i)checksum+=business.step();auto warm_remote_cpu=main_probe.cpu();task.store(1,std::memory_order_release);while(task.load(std::memory_order_acquire))TwoBufQueue::cpu_relax();auto b=remote_record;
  if(k)std::cout<<',';
  const auto emit=[&](const Record&r,std::size_t warm_cpu){std::cout<<"{\"warm_cpu\":"<<warm_cpu<<",\"cpu_begin\":"<<r.before_cpu<<",\"cpu_end\":"<<r.after_cpu<<",\"groups\":[";for(int j=0;j<4;++j){if(j)std::cout<<',';std::cout<<r.group_ns[j];}std::cout<<"],\"instructions\":"<<r.counters.instructions<<",\"cycles\":"<<r.counters.cycles<<",\"user_ns\":"<<r.counters.user_ns<<",\"kernel_ns\":"<<r.counters.system_ns<<'}';};
  std::cout<<"{\"local\":";emit(a,warm_local_cpu);std::cout<<",\"remote\":";emit(b,warm_remote_cpu);std::cout<<'}';
 }
 task.store(2,std::memory_order_release);worker.join();std::cout<<"],\"checksum\":"<<checksum<<"}\n";
}
