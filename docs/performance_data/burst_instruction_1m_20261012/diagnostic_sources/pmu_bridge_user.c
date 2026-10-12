// Direct per-thread snapshots. Reuse the installed, pinned mperf loader/event allocator.
#define main mperf_unused_main
#include "/Users/lc/Documents/Codex/2026-10-08/twobufqueue/work/burst_instruction_20261012/mperf_user_only.c"
#undef main
static pmc_state_t brq_state;
static int brq_owned;
static u32 brq_old_counting, brq_old_thread;
static kpc_config_t brq_old_regs[KPC_MAX_COUNTERS];
static int (*brq_get_config)(u32,kpc_config_t*);
static u32 (*brq_get_counting)(void);
static u32 (*brq_get_thread_counting)(void);
static const char *brq_load[]={"cycles","instructions","L1D_CACHE_MISS_LD_NONSPEC","L1D_CACHE_MISS_ST_NONSPEC","L1D_CACHE_WRITEBACK","L1D_TLB_MISS_NONSPEC",NULL,NULL};
static const char *brq_store[]={"cycles","instructions","MAP_STALL","MAP_STALL_DISPATCH","MAP_REWIND","ST_MEMORY_ORDER_VIOLATION_NONSPEC","LD_UNIT_UOP","ST_UNIT_UOP"};
void brq_pmu_close(void) {
 if (brq_owned) {
  kpc_set_thread_counting(0); kpc_set_counting(0);
  if (brq_get_config) kpc_set_config(brq_state.classes,brq_old_regs);
  kpc_set_counting(brq_old_counting); kpc_set_thread_counting(brq_old_thread);
  kpc_force_all_ctrs_set(0);brq_owned=0;
 }
 pmc_cleanup(&brq_state); memset(&brq_state,0,sizeof(brq_state));
}
int brq_pmu_open(const char *group,int plan_only) {
 if (!load_frameworks()) return 1;
 const char **events=!strcmp(group,"cache")?brq_load:!strcmp(group,"pipeline")?brq_store:NULL;
 if(!events) return 2;
 memset(&brq_state,0,sizeof(brq_state));
 if(kpep_db_create(NULL,&brq_state.db)||kpep_config_create(brq_state.db,&brq_state.config)||kpep_config_force_counters(brq_state.config)) return 3;
 for(int i=0;i<8 && events[i];i++) if(!pmc_add_event(&brq_state,events[i])){brq_pmu_close();return 4;}
 if(kpep_config_kpc_classes(brq_state.config,&brq_state.classes)||
    kpep_config_kpc_count(brq_state.config,&brq_state.reg_count)||
    kpep_config_kpc_map(brq_state.config,brq_state.counter_map,sizeof(brq_state.counter_map))||
    kpep_config_kpc(brq_state.config,brq_state.regs,sizeof(brq_state.regs))){brq_pmu_close();return 5;}
 brq_state.counter_count=kpc_get_counter_count(brq_state.classes);
 if(plan_only) return 0;
 if(geteuid()!=0){fputs("PMU counter setup requires administrator authentication\n",stderr);brq_pmu_close();return 6;}
 int forced=0;
 if(kpc_force_all_ctrs_get(&forced)||forced){fputs("PMU counters unavailable or already owned by another profiler\n",stderr);brq_pmu_close();return 7;}
 brq_get_config=dlsym(lib_kperf,"kpc_get_config");
 brq_get_counting=dlsym(lib_kperf,"kpc_get_counting");
 brq_get_thread_counting=dlsym(lib_kperf,"kpc_get_thread_counting");
 if(!brq_get_config||!brq_get_counting||!brq_get_thread_counting){brq_pmu_close();return 8;}
 brq_old_counting=brq_get_counting();brq_old_thread=brq_get_thread_counting();
 if(brq_get_config(brq_state.classes,brq_old_regs)){brq_pmu_close();return 9;}
 if(kpc_force_all_ctrs_set(1)){brq_pmu_close();return 10;}
 brq_owned=1;
 if(kpc_set_config(brq_state.classes,brq_state.regs)||kpc_set_counting(brq_state.classes)||kpc_set_thread_counting(brq_state.classes)){brq_pmu_close();return 11;}
 usleep(1000);
 return 0;
}
int brq_pmu_read(uint64_t *out) {
 uint64_t raw[KPC_MAX_COUNTERS]={0};
 int result=kpc_get_thread_counters(0,brq_state.counter_count,raw);
 if(result) return result;
 for(int i=0;i<brq_state.event_count;i++)out[i]=raw[brq_state.counter_map[i]];
 return 0;
}
const char *brq_pmu_name(int i){return brq_state.events[i].name;}
const char *brq_pmu_description(int i){return brq_state.events[i].event->description;}
int brq_pmu_count(void){return brq_state.event_count;}
int brq_pmu_cleanup_stale(void){
 if(!load_frameworks()||geteuid()!=0)return 1;
 return kpc_set_thread_counting(0)||kpc_set_counting(0)||kpc_force_all_ctrs_set(0);
}
