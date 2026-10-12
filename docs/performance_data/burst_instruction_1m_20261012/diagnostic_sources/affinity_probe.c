#include <stdio.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
int main(void){thread_affinity_policy_data_t p={1};thread_port_t t=mach_thread_self();kern_return_t r=thread_policy_set(t,THREAD_AFFINITY_POLICY,(thread_policy_t)&p,THREAD_AFFINITY_POLICY_COUNT);printf("thread_affinity_policy=%d (%s)\n",r,mach_error_string(r));if(!r){p.affinity_tag=0;printf("restore=%d\n",thread_policy_set(t,THREAD_AFFINITY_POLICY,(thread_policy_t)&p,THREAD_AFFINITY_POLICY_COUNT));}mach_port_deallocate(mach_task_self(),t);return 0;}
