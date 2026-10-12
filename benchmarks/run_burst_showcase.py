#!/usr/bin/env python3
"""One command: build, calibrate a common slow callback, select business size,
collect enough immediate-post-burst samples, and save the final HTML report.

Use --replay selected_config.json to rerun the recorded parameters without a
new selection. --resume continues a matching incomplete workflow. Quantiles
are per16-query group means, averaged across independent runs, not single calls.
"""
import argparse
import hashlib
import json
import math
import os
import pathlib
import platform
import shutil
import statistics
import subprocess
import sys
import time
from types import SimpleNamespace
import tune_burst_business as tuning
from burst_showcase_report import render_report
from burst_replay import compiler_info, choose_compiler, capture_queue_parameters, append_queue_parameters

MIB = 1024*1024
ROOT = pathlib.Path(__file__).resolve().parents[1]


def machine_info():
    info = {'cpu': platform.processor() or platform.machine(),
            'os': platform.platform(), 'architecture': platform.machine(),
            'producer_qos': 'user_interactive' if platform.system() == 'Darwin' else 'unchanged',
            'hard_affinity': False}
    if platform.system() == 'Darwin':
        def read(key):
            try:
                value = subprocess.run(['/usr/sbin/sysctl', '-n', key], text=True,
                    capture_output=True, timeout=2)
                return value.stdout.strip() if value.returncode == 0 else None
            except (OSError, subprocess.TimeoutExpired):
                return None
        info['cpu'] = read('machdep.cpu.brand_string') or 'Apple Silicon (' + platform.machine() + ')'
        for field, key in [('model','hw.model'), ('os_build','kern.osversion')]:
            value = read(key)
            if value: info[field] = value
        info['os_version'] = platform.mac_ver()[0]
        for field, key in [('memory_bytes','hw.memsize'), ('physical_cores','hw.physicalcpu'),
                           ('logical_cores','hw.logicalcpu')]:
            value = read(key)
            if value and value.isdecimal(): info[field] = int(value)
        for index in range(2):
            name = read(f'hw.perflevel{index}.name')
            count = read(f'hw.perflevel{index}.physicalcpu')
            if name and count and count.isdecimal() and name.lower() in ('performance','efficiency'):
                info[name.lower() + '_cores'] = int(count)
    return info


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary=path.with_suffix(path.suffix+'.tmp')
    temporary.write_text(json.dumps(value,indent=2)+'\n')
    temporary.replace(path)


def build(args):
    if args.executable:
        return args.executable.resolve()
    deps=args.deps.resolve()
    subprocess.run([sys.executable,str(ROOT/'benchmarks/bootstrap_spsc.py'),
                    '--output',str(deps)],check=True)
    cmake=shutil.which('cmake')
    if not cmake: raise RuntimeError('cmake is required to build the benchmark')
    build_dir=args.build_dir.resolve()
    configure=[cmake,'-S',str(ROOT),'-B',str(build_dir),'-DCMAKE_BUILD_TYPE=Release',
               '-DBRQ_BUILD_TESTS=OFF','-DBRQ_BUILD_EXAMPLES=OFF',
               '-DBRQ_BUILD_SPSC_BENCHMARKS=ON','-DBRQ_ENABLE_POOL_DEPTH_STATS=OFF',
               '-DBRQ_SPSC_DEPS='+str(deps)]
    if args.compiler: configure+=['-DCMAKE_CXX_COMPILER='+args.compiler]
    for name in ('CMAKE_CXX_FLAGS','CMAKE_CXX_FLAGS_RELEASE'):
        recorded=getattr(args,'recorded_build',{})
        if name in recorded:configure+=['-D'+name+'='+recorded[name]]
    subprocess.run(configure,check=True)
    subprocess.run([cmake,'--build',str(build_dir),'--target','test_burst_cache','-j',str(args.jobs)],check=True)
    executable=build_dir/('test_burst_cache.exe' if os.name=='nt' else 'test_burst_cache')
    subprocess.run([str(executable),'--self-test'],check=True)
    return executable


def validate(data, business, bursts, repeats, loops, regular=True):
    if not data.get('verified') or data['accepted']!=data['completed']:
        raise ValueError('message/FIFO/count validation failed')
    if data['business_bytes']!=business*1024 or data['queue_config']['callback_iterations']!=loops:
        raise ValueError('wrong business or callback workload')
    expected=[b*MIB for b in bursts for _ in range(repeats)]
    if [p['burst_bytes'] for p in data['points']]!=expected:
        raise ValueError('missing burst windows')
    if data['formal']!=sum(bursts)*MIB//32*repeats:
        raise ValueError('wrong formal payload count')
    if data['query_group']!=16 or data['query_groups']!=4:
        raise ValueError('post-burst window must be64queries in4groups of16')
    if any(len(p['after_business_ns'])!=4 or any(not math.isfinite(x) or x<0 for x in p['after_business_ns']) for p in data['points']):
        raise ValueError('invalid timing samples')
    if platform.system()=='Darwin' and not data['qos_applied']:
        raise ValueError('producer QoS was not verified')
    if data['transport']=='two_buf_queue':
        q=data['queue_config']
        if q['source_pool_max']*q['source_block_size']!=16384 or q['output_pool_max']*q['output_block_size']!=256*MIB:
            raise ValueError('wrong TwoBufQueue layer capacities')
    if regular and data['transport']!='baseline':
        if not all(p['setup_target_reached'] for p in data['points']):
            raise ValueError('occupancy preparation did not reach its target')
        if min(p['pending_after'] for p in data['points'])/(256*MIB)<.70:
            raise ValueError('actual pending data fell below70%; calibration must be checked')


def run_case(executable, folder, business, case, run, bursts, repeats, loops, timeout, resume, progress=0, queue_parameters=None, benchmark_parameters=None):
    label,transport,flags=case
    options=SimpleNamespace(callback_iterations=loops,window_repeats=repeats,
                            bursts_mib=bursts,process_timeout=timeout)
    cmd=tuning.arguments(executable,transport,flags,business,options)
    for key,value in (benchmark_parameters or {}).items():
        cmd+=['--'+key,str(value).lower() if isinstance(value,bool) else str(value)]
    cmd+=['--progress_every',str(progress)]
    cmd=append_queue_parameters(cmd,queue_parameters)
    key=hashlib.sha256(json.dumps({'binary':tuning.sha(executable),'argv':cmd},sort_keys=True).encode()).hexdigest()
    target=folder/'raw'/f'{business}k_{label}_run{run}.json'
    if target.exists():
        if not resume: raise ValueError(f'refusing to replace completed result: {target}')
        data=json.loads(target.read_text())
        if data['invocation'].get('key')!=key: raise ValueError('resume parameters or executable changed')
        validate(data,business,bursts,repeats,loops,regular=False)
        print(f'resumed {folder.name}: {business}KiB {label} run{run}',flush=True)
        return data
    target.parent.mkdir(parents=True,exist_ok=True)
    temporary=target.with_suffix('.stdout.tmp')
    log=target.with_suffix('.stderr.log')
    begin=time.monotonic();last=''
    with temporary.open('w') as stdout,log.open('w') as stderr:
        process=subprocess.Popen(cmd,stdout=stdout,stderr=stderr,text=True)
        try:
            while process.poll() is None:
                if time.monotonic()-begin>timeout+20:
                    process.kill();process.wait();raise RuntimeError(f'{label} exceeded process timeout')
                lines=log.read_text().splitlines()
                line=lines[-1] if lines else ''
                if line and line!=last:
                    print(f'{folder.name}: {business}KiB {label} run{run} {line}',flush=True);last=line
                time.sleep(1)
        except BaseException:
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait()
            raise
    if process.returncode:
        raise RuntimeError(f'{label} exit{process.returncode}: {log.read_text()}')
    data=json.loads(temporary.read_text());validate(data,business,bursts,repeats,loops,regular=False)
    data['invocation']={'argv':cmd,'key':key,'wall_seconds':time.monotonic()-begin,
                        'utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'stderr_file':log.name}
    save(target,data);temporary.unlink()
    print(f'{folder.name}: {business}KiB {label} run{run} finished '
          f'{data["invocation"]["wall_seconds"]:.1f}s, {len(data["points"])*4} business groups',flush=True)
    return data


def calibrate(executable, folder, settings, resume):
    path=folder/'calibration.json'
    if path.exists() and resume:
        result=json.loads(path.read_text())
        if result['executable_sha256']!=tuning.sha(executable):raise ValueError('calibration binary changed')
        return result['callback_iterations']
    observations=[]
    for loops in settings['callback_candidates']:
        minimum=1.;ready=True
        for business in settings['business_kib']:
            for case in tuning.CASES[1:]:
                data=run_case(executable,folder/f'loops{loops}',business,case,1,
                              [max(settings['bursts_mib'])],3,loops,settings['process_timeout'],resume)
                occupancy=min(p['pending_after'] for p in data['points'])/(256*MIB)
                reached=all(p['setup_target_reached'] for p in data['points'])
                minimum=min(minimum,occupancy);ready &= reached
                observations.append({'business_kib':business,'queue':case[0],'loops':loops,
                                     'minimum_occupancy':occupancy,'prepared':reached})
        save(folder/'trials.json',observations)
        if ready and minimum>=.75:
            result={'callback_iterations':loops,'minimum_occupancy':minimum,
                    'rule':'smallest common candidate retaining75%pending after3largest bursts at every business size and queue',
                    'observations':observations,'executable_sha256':tuning.sha(executable)}
            save(path,result)
            print(f'common callback selected: {loops} loops; pilot minimum pending={minimum:.1%}',flush=True)
            return loops
    raise RuntimeError('No common callback satisfies the occupancy requirement')


def phase(executable,folder,business_sizes,settings,repeats,loops,resume,queue_parameters=None,benchmark_parameters=None):
    manifest_path=folder/'manifest.json'
    manifest={'schema':'burst_business_sweep_v1','business_kib':business_sizes,
              'bursts_mib':settings['bursts_mib'],'independent_runs':settings['runs'],
              'window_repeats':repeats,'writer_pool_total_bytes':16384,
              'reader_pool_total_bytes':256*MIB,'callback_iterations':loops,
              'post_window_queries':64,'queries_per_timing_group':16,
              'executable_sha256':tuning.sha(executable),'records':[]}
    definitions=[(b,c) for b in business_sizes for c in tuning.CASES]
    for run in range(1,settings['runs']+1):
        offset=(run-1)*5%len(definitions)
        for business,case in definitions[offset:]+definitions[:offset]:
            data=run_case(executable,folder,business,case,run,settings['bursts_mib'],
                          repeats,loops,settings['process_timeout'],resume,
                          progress=128 if repeats>128 else 0,
                          queue_parameters=(queue_parameters or {}).get(case[0]),
                          benchmark_parameters=benchmark_parameters)
            validate(data,business,settings['bursts_mib'],repeats,loops)
            manifest['records'].append({'business_kib':business,'queue':case[0],
                                        'run':run,'file':f'raw/{business}k_{case[0]}_run{run}.json'})
            save(manifest_path,manifest)
    return tuning.aggregate(folder)


def select(summary):
    scores=summary['scores']
    qualified=[s for s in scores if s['winning_comparisons']>s['comparisons']/2
               and sum(x>1 for x in s['per_run_scores'])>=2] or scores
    top=max(s['advantage_geometric_mean'] for s in qualified)
    plateau=[s for s in qualified if s['advantage_geometric_mean']>=top*.98]
    def order(s):
        cv=statistics.stdev(s['per_run_scores'])/statistics.mean(s['per_run_scores']) if len(s['per_run_scores'])>1 else 0.
        return (-s['winning_comparisons'],cv,s['business_kib'])
    chosen=min(plateau,key=order)
    return {'business_kib':chosen['business_kib'],'scores':scores,
            'rule':'use median-window relative advantages over all burst sizes and runs; among candidates within2%of best score choose most wins, lower run variability, then smaller working set'}


def nearest(values,p):
    if not values:raise ValueError('missing quantile samples')
    ordered=sorted(values)
    return ordered[math.ceil(len(ordered)*p)-1]


def final_summary(folder,profile):
    manifest=json.loads((folder/'manifest.json').read_text())
    rows=[];checksums=set()
    for case in tuning.CASES:
        label=case[0];per_burst={b:[] for b in profile['bursts_mib']}
        for run in range(1,profile['runs']+1):
            data=json.loads((folder/f'raw/{profile["selected_business_kib"]}k_{label}_run{run}.json').read_text())
            checksums.add(data['business_checksum'])
            validate(data,profile['selected_business_kib'],profile['bursts_mib'],
                     profile['final_window_repeats'],profile['callback_iterations'])
            for burst in profile['bursts_mib']:
                points=[p for p in data['points'] if p['burst_bytes']==burst*MIB]
                samples=[x for p in points for x in p['after_business_ns']]
                if len(samples)<profile['samples_per_run']:raise ValueError('insufficient tail samples')
                per_burst[burst].append({'run':run,'before_ns':statistics.mean(p['pre_business_ns'] for p in points),
                                        'p50':nearest(samples,.5),'p99':nearest(samples,.99),'p999':nearest(samples,.999),
                                        'samples':len(samples),'occupancy_min':min(p['pending_after'] for p in points)/(256*MIB),
                                        'occupancy_mean':statistics.mean(p['pending_after'] for p in points)/(256*MIB),
                                        'backpressured_messages':sum(p['backpressured_messages'] for p in points),
                                        'formal_messages':sum(p['burst_bytes']//32 for p in points)})
        for burst,rr in per_burst.items():
            rows.append({'queue':label,'burst_mib':burst,
                         **{key:statistics.mean(x[key] for x in rr) for key in ('before_ns','p50','p99','p999')},
                         'samples_per_run':rr[0]['samples'],'per_run':rr})
    if len(checksums)!=1:raise ValueError('business checksum differs between queues/runs')
    result={'schema':'burst_showcase_result_v1','parameters':profile,'rows':rows,
            'verified':True,'business_checksum':checksums.pop(),
            'timer_name':data.get('timer_name','recorded in raw data'),
            'timer_quantum_ns':data.get('timer_quantum_ns',0),
            'quantile_definition':'nearest-rank per16-query group means from immediate first64post-burst queries; quantiles computed perindependent run then arithmetic mean'}
    save(folder/'report_results.json',result)
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable',type=pathlib.Path)
    p.add_argument('--output',type=pathlib.Path,default=ROOT/'docs/performance_data'/('burst_showcase_'+time.strftime('%Y%m%d_%H%M%S')))
    p.add_argument('--report',type=pathlib.Path,default=ROOT/'docs/performance_burst_cache.html')
    p.add_argument('--deps',type=pathlib.Path,default=ROOT/'.deps/spsc')
    p.add_argument('--build-dir',type=pathlib.Path,default=ROOT/'build-burst-showcase')
    p.add_argument('--compiler')
    p.add_argument('--jobs',type=int,default=4)
    p.add_argument('--business-kib',type=tuning.csv_int,default=[24,48,96])
    p.add_argument('--bursts-mib',type=tuning.csv_int,default=[1,2,4,8,16])
    p.add_argument('--runs',type=int,default=3)
    p.add_argument('--selection-repeats',type=int,default=8)
    p.add_argument('--samples-per-run',type=int,default=10000)
    p.add_argument('--callback-candidates',type=tuning.csv_int,default=[16,32,64,128,256])
    p.add_argument('--callback-iterations',type=int,default=0)
    p.add_argument('--process-timeout',type=int,default=900)
    p.add_argument('--resume',action='store_true')
    p.add_argument('--replay',type=pathlib.Path)
    p.add_argument('--render-only',action='store_true')
    args=p.parse_args();folder=args.output.resolve();report=args.report.resolve()
    if args.render_only:
        result=json.loads((folder/'formal/report_results.json').read_text());render_report(folder,report,result);return
    if args.samples_per_run<10000:p.error('formal p999 requires at least10000business-group samples perrun')
    if args.runs<3 or args.selection_repeats<1 or args.process_timeout<30:p.error('use at least3runs and positive repeat/timeout values')
    replay_input=json.loads(args.replay.read_text()) if args.replay else None
    args.recorded_build=(replay_input or {}).get('build',{})
    args.compiler=choose_compiler(args.compiler,replay_input)
    executable=build(args)
    if not executable.exists():raise ValueError('executable does not exist')
    folder.mkdir(parents=True,exist_ok=True)
    if (folder/'workflow.json').exists() and not args.resume:p.error('output already contains a workflow; use--resume or a fresh output directory')
    settings={'business_kib':args.business_kib,'bursts_mib':args.bursts_mib,'runs':args.runs,
              'selection_repeats':args.selection_repeats,'samples_per_run':args.samples_per_run,
              'callback_candidates':args.callback_candidates,'process_timeout':args.process_timeout,
              'fixed_callback_iterations':args.callback_iterations}
    replay_profile=replay_input
    if replay_profile:
        for key in ('bursts_mib','runs','samples_per_run','process_timeout'):
            settings[key]=replay_profile[key]
        if replay_profile['samples_per_run']<10000 or replay_profile['runs']<3:
            raise ValueError('replay profile has insufficient samples or runs')
    source_dir=pathlib.Path(__file__).parent
    sources={str(f.relative_to(source_dir)):tuning.sha(f) for f in source_dir.rglob('*') if f.is_file() and f.suffix in ('.hpp','.cpp','.py')}
    workflow={'schema':'burst_showcase_workflow_v1','settings':settings,'executable_sha256':tuning.sha(executable),
              'source_sha256':sources,'platform':platform.platform(),'consumer_pause':False,
              'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'status':'running',
              'build':compiler_info(executable),
              'library_sha256':{str(f.relative_to(ROOT)):tuning.sha(f)
                 for directory in ('src','include') for f in (ROOT/directory).rglob('*')
                 if f.is_file() and f.suffix in ('.h','.hpp','.cpp')}}
    if args.resume:
        old=json.loads((folder/'workflow.json').read_text())
        if old['settings']!=settings or old['executable_sha256']!=workflow['executable_sha256']:raise ValueError('resume settings/binary changed')
        workflow=old
    save(folder/'workflow.json',workflow)
    if not args.resume or not (folder/'environment.json').exists():
        save(folder/'environment.json', {'machine': machine_info(), 'source': 'test host at benchmark startup'})
    if args.replay:
        profile=replay_profile
        workflow['replay_profile']=str(args.replay.resolve())
    else:
        loops=args.callback_iterations or calibrate(executable,folder/'calibration',settings,args.resume)
        selection=phase(executable,folder/'selection',settings['business_kib'],settings,
                        settings['selection_repeats'],loops,args.resume)
        chosen=select(selection);save(folder/'selection/choice.json',chosen)
        profile={**settings,'selected_business_kib':chosen['business_kib'],'callback_iterations':loops,
                 'final_window_repeats':math.ceil(settings['samples_per_run']/4),
                 'writer_pool_total_bytes':16384,'reader_pool_total_bytes':256*MIB,
                 'native_slot_storage_bytes':256*MIB,'payload_bytes':32,'business_accesses':8,
                 'business_warm_queries':512,'pre_queries':16,'post_queries':64,'group_queries':16,
                 'quantiles':'perrunnearest-rank,then3runarithmeticmean',
                 'selection_rule':chosen['rule'],'machine':machine_info(),'regular_apis':{'ReaderWriterQueue':'try_enqueue+membercopy','rigtorp':'try_push;noaddedprefetch','atomic_queue':'SPSCtry_push'},
                 'producer_qos':'user_interactive on macOS','producer_cpu':-1,
                 'publish_count':4294967295,'scan_current':False}
    frozen=folder/'selected_config.json'
    if args.resume and frozen.exists():
        profile=json.loads(frozen.read_text())
    elif not args.replay:
        profile['queue_parameters']=capture_queue_parameters(folder/'selection',profile['selected_business_kib'],[c[0] for c in tuning.CASES])
        profile['build']=compiler_info(executable)
        raw=json.loads((folder/f'selection/raw/{profile["selected_business_kib"]}k_TwoBufQueue_run1.json').read_text())
        profile['benchmark_parameters']={key:raw[key] for key in (
            'source_pool_bytes','business_accesses','warm_queries','query_group',
            'query_groups','target_percent','setup_timeout_ms','prefill')}
    save(frozen,profile)
    print(f'formal measurement: {profile["selected_business_kib"]}KiB business, '
          f'{profile["callback_iterations"]}callback loops, '
          f'{profile["final_window_repeats"]}bursts/size/run, '
          f'{profile["samples_per_run"]}groups/size/run',flush=True)
    phase(executable,folder/'formal',[profile['selected_business_kib']],settings,
          profile['final_window_repeats'],profile['callback_iterations'],args.resume,
          profile.get('queue_parameters'),profile.get('benchmark_parameters'))
    result=final_summary(folder/'formal',profile)
    render_report(folder,report,result)
    workflow['status']='complete';workflow['finished_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
    workflow['report']=str(report);save(folder/'workflow.json',workflow)
    print('Saved complete report:',report,flush=True)


if __name__=='__main__':
    try:main()
    except (RuntimeError,ValueError,KeyError) as error:
        print('burst showcase:',error,file=sys.stderr);sys.exit(1)
