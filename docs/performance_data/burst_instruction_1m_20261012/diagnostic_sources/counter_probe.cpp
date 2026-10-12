#include <dlfcn.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <vector>
#include <algorithm>
struct Counts{std::uint64_t instructions,cycles,user,system;};
using Read=int(*)(std::uint32_t,void*,std::size_t);
int main(){auto f=(Read)dlsym(RTLD_DEFAULT,"thread_selfcounts");if(!f){puts("not found");return 1;}mach_timebase_info_data_t tb;mach_timebase_info(&tb);double ns=double(tb.numer)/tb.denom;
 for(auto kind:{1u,2u,3u,4u}){Counts c[2]{};size_t size=kind==1?16:kind==2?32:kind==3?32:64;errno=0;int r=f(kind,c,size);printf("kind%u status=%d errno=%d instr=%llu cycles=%llu user=%llu system=%llu\n",kind,r,errno,c[0].instructions,c[0].cycles,c[0].user,c[0].system);}
 std::vector<double> costs;costs.reserve(10000);Counts c{};for(int i=0;i<10000;++i){auto a=mach_absolute_time();if(f(3,&c,sizeof(c)))return 2;auto b=mach_absolute_time();costs.push_back((b-a)*ns);}std::sort(costs.begin(),costs.end());printf("snapshot cost p50=%.2fns p99=%.2fns p999=%.2fns\n",costs[4999],costs[9899],costs[9989]);
 for(int i=0;i<5;++i){Counts a{},b{};f(3,&a,sizeof(a));asm volatile("" ::: "memory");f(3,&b,sizeof(b));printf("empty delta instr=%llu cycles=%llu user_ns=%.1f kernel_ns=%.1f\n",b.instructions-a.instructions,b.cycles-a.cycles,(b.user-a.user)*ns,(b.system-a.system)*ns);}
}
