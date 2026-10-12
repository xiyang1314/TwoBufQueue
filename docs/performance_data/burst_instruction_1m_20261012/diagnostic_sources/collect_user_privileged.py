#!/usr/bin/python3
"""Frozen, bounded per-window PMU collection; no repository imports or shell."""
import json,hashlib,os,pathlib,subprocess,time
ROOT=pathlib.Path(__file__).resolve().parent
BINARY=ROOT/'build-user/test_burst_cache'
PROFILE=ROOT/'frozen_profile.json'
EXPECTED_BINARY='86803b8e01f68fc2648d8f0a1e14d951312f1400353751e9c87f2a96e2132ca4'
EXPECTED_PROFILE='69faffce5eb8a3d59f75a74da624824b2d255095fb9cb2a20336071a5da9032a'

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    if os.geteuid()!=0:raise RuntimeError('PMU configuration requires administrator authentication')
    if digest(BINARY)!=EXPECTED_BINARY or digest(PROFILE)!=EXPECTED_PROFILE:raise RuntimeError('prepared executable/profile changed')
    # Wait for the independent low-overhead CPU-position experiment to finish.
    deadline=time.monotonic()+600
    while not (ROOT/'cpu_capture.complete').exists():
        if time.monotonic()>deadline:raise RuntimeError('CPU-position experiment did not complete')
        time.sleep(1)
    profile=json.loads(PROFILE.read_text())
    cases=[('TwoBufQueue','two_buf_queue'),('ReaderWriterQueue','readerwriterqueue'),('baseline','baseline')]
    groups={'cache':['cycles','instructions','L1D_CACHE_MISS_LD_NONSPEC','L1D_CACHE_MISS_ST_NONSPEC','L1D_CACHE_WRITEBACK','L1D_TLB_MISS_NONSPEC'],
            'pipeline':['cycles','instructions','MAP_STALL','MAP_STALL_DISPATCH','MAP_REWIND','ST_MEMORY_ORDER_VIOLATION_NONSPEC','LD_UNIT_UOP','ST_UNIT_UOP']}
    output=ROOT/('pmu_user_'+time.strftime('%Y%m%d_%H%M%S'));output.mkdir()
    metadata={'scope':'USER_ONLY;writer_current_thread;onlyimmediate64querywindow;instrumenteddiagnosis','profile':profile,'groups':groups,'executable_sha256':digest(BINARY),'collector_sha256':digest(pathlib.Path(__file__)),'records':[]}
    (output/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    for run in range(1,4):
        for group,names in groups.items():
            offset=(run-1)%len(cases)
            for name,transport in cases[offset:]+cases[:offset]:
                stem=f'{group}_{name}_run{run}'
                cmd=[str(BINARY),'--transport',transport,'--probe_mode','3','--source_pool_bytes','16384','--business_bytes','49152','--repeats','2500','--burst_sizes','1048576','--progress_every','512']
                values=dict(profile['benchmark_parameters']);values.update({k:v for k,v in profile['queue_parameters'][name].items() if k!='transport'})
                for k,v in values.items():cmd+=['--'+k,str(v).lower() if isinstance(v,bool) else str(v)]
                if digest(BINARY)!=EXPECTED_BINARY:raise RuntimeError('binary changed mid-batch')
                print(f'PMU {group} {name} run{run}',flush=True)
                env=dict(os.environ);env['BRQ_PMU_GROUP']=group
                with (output/(stem+'.stdout.tmp')).open('w') as out,(output/(stem+'.stderr.log')).open('w') as err:
                    proc=subprocess.Popen(cmd,stdout=out,stderr=err,env=env)
                    try:code=proc.wait(timeout=920)
                    except BaseException:
                        proc.terminate()
                        try:proc.wait(timeout=5)
                        except subprocess.TimeoutExpired:proc.kill();proc.wait()
                        raise
                if code:raise RuntimeError(f'PMU case failed: {stem};see stderr')
                raw=json.loads((output/(stem+'.stdout.tmp')).read_text())
                if not raw['verified'] or raw['accepted']!=raw['completed'] or raw['pmu_group']!=group or len(raw['points'])!=2500:raise RuntimeError('PMU validation failed')
                if any(not pt['pmu_counts'][0] or not pt['pmu_counts'][1] for pt in raw['points']):raise RuntimeError('PMU returned zero counters')
                raw['invocation']={'argv':cmd,'events':names,'executable_sha256':EXPECTED_BINARY}
                (output/(stem+'.json')).write_text(json.dumps(raw,indent=2)+'\n');(output/(stem+'.stdout.tmp')).unlink()
                metadata['records'].append({'queue':name,'group':group,'run':run,'file':stem+'.json'})
                (output/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    metadata['complete']=True;(output/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    uid=ROOT.stat().st_uid;gid=ROOT.stat().st_gid
    for path in output.rglob('*'):os.chown(path,uid,gid)
    os.chown(output,uid,gid)
    print('Saved: '+str(output),flush=True)
if __name__=='__main__':main()
