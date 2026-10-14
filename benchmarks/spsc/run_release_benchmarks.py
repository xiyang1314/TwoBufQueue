"""Reproducible fixed-parameter ordinary-write / WriteSession reports (no downloads)."""
from pathlib import Path
import argparse, os, datetime, hashlib, json, platform, random, shutil, statistics, subprocess, sys, tarfile, time
import tune_spsc as tune
from spsc_html_report import write_html_report
R = Path(__file__).resolve().parents[2]

def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def power():
    return subprocess.check_output(['pmset','-g','batt'], text=True) if sys.platform=='darwin' else 'Unavailable on this platform\n'
def source_files():
    files=[R/'CMakeLists.txt']
    for directory in ('src','include','support','test','examples','cmake'):
        files += [p for p in (R/directory).rglob('*') if p.is_file() and
                  (p.suffix in ('.hpp','.cpp','.py','.json','.cmake','.in') or p.name=='CMakeLists.txt')]
    files += [R/'LICENSE', R/'NOTICE']
    files += [p for p in (R/'benchmarks').rglob('*') if p.is_file() and p.suffix in ('.py','.cpp','.hpp','.cmake','.json')]
    files += list((R/'benchmarks/tunable').glob('*.hpp')) + list((R/'benchmarks/tunable').glob('*.cpp'))
    files += [R/'.github/workflows/ci.yml', R/'benchmarks/profiles/spsc_release.json']
    return sorted(set(files))

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--deps', type=Path, default=R/'.deps/spsc')
parser.add_argument('--output', type=Path, required=True, help='New immutable results directory')
parser.add_argument('--report-dir', type=Path, default=R/'docs/reports/comparison')
parser.add_argument('--profile', type=Path, default=R/'benchmarks/profiles/spsc_release.json')
parser.add_argument('--timer', choices=['auto','steady'], default='auto')
parser.add_argument('--write-mode', choices=['normal','session','both'], default='both', help='Only regenerate the selected reports; native controls are freshly measured')
parser.add_argument('--callback-iterations', type=int, nargs=4, metavar=('B8','B16','B32','B64'), help='Fixed 256 MiB loop counts for 8/16/32/64 B; skip recalibration; 8 MiB remains zero')
parser.add_argument('--hardware-info', type=Path, help='Optional independently collected hardware metadata JSON')
parser.add_argument('--copy-sensitivity', type=Path, help='Optional independent copy-constructor control summary')
parser.add_argument('--resume', action='store_true', help='Requires identical executable, sources and settings')
parser.add_argument('--messages', type=int, default=10000000)
parser.add_argument('--repetitions', type=int, default=3)
parser.add_argument('--cache', type=int, nargs='+', choices=[8,256], default=[8,256])
parser.add_argument('--payload', type=int, nargs='+', choices=[8,16,32,64], default=[8,16,32,64])
parser.add_argument('--producer-cpu', type=int, default=-1)
parser.add_argument('--producer-qos', choices=['unchanged','user_interactive'], default='user_interactive' if sys.platform=='darwin' else 'unchanged')
args=parser.parse_args()
if not __debug__: raise RuntimeError("Run without Python -O; audit assertions must remain enabled")
if args.messages<1000 or args.repetitions<1: parser.error('messages >= 1000 and repetitions >= 1 required')
if args.callback_iterations is not None and min(args.callback_iterations)<0: parser.error('callback iterations must be nonnegative')
write_modes=('normal','session') if args.write_mode=='both' else (args.write_mode,)
fixed_loops=dict(zip((8,16,32,64),args.callback_iterations)) if args.callback_iterations is not None else None
B=args.binary.resolve(); ROOT=args.output.resolve(); args.report_dir=args.report_dir.resolve()
ROOT.mkdir(parents=True,exist_ok=args.resume); args.report_dir.mkdir(parents=True,exist_ok=True)
STAMP=ROOT.name
settings=json.loads(args.profile.read_text(encoding='utf-8'))
# Older saved profiles predate this option. Make its unchanged legacy default
# explicit so the complete serialized configuration matches recorded arguments.
for cache_configs in settings['caches'].values():
    for config in cache_configs.values():
        config.setdefault('fixed_round_boundaries', False)
if sys.platform=='darwin' and 'AC Power' not in power(): raise RuntimeError('Connect AC power before formal measurements')
deps=json.loads((R/'benchmarks/spsc/spsc_dependencies.json').read_text(encoding='utf-8'))
for dep, definition in deps.items():
    for file in definition['files']:
        if sha(args.deps/dep/file['path'])!=file['sha256']: raise ValueError('Dependency hash mismatch: '+dep+'/'+file['path'])
build=json.loads(subprocess.check_output([str(B),'--build-info'],text=True))
if not build['optimized'] or build['sanitized']: raise ValueError('Formal measurements require optimized, unsanitized executable')
files=source_files(); sources={str(p.relative_to(R)):sha(p) for p in files}
try:
    commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True,stderr=subprocess.DEVNULL).strip()
    dirty=bool(subprocess.check_output(['git','status','--porcelain'],cwd=R,text=True,stderr=subprocess.DEVNULL))
except (OSError,subprocess.CalledProcessError): commit=None;dirty=None
cli={k:v for k,v in vars(args).items() if k not in ('binary','deps','output','report_dir','profile','resume','hardware_info','copy_sensitivity')}
hardware={}
if sys.platform=='darwin':
    for key in ('machdep.cpu.brand_string','hw.memsize','hw.physicalcpu','hw.logicalcpu'):
        query=subprocess.run(['sysctl','-n',key],capture_output=True,text=True)
        hardware[key]=query.stdout.strip() if query.returncode==0 else {'unavailable':query.stderr.strip()}
if args.hardware_info:
    hardware=json.loads(args.hardware_info.read_text(encoding='utf-8'))
    if not isinstance(hardware,dict): raise ValueError('hardware-info must be a JSON object')
copy_sensitivity=json.loads(args.copy_sensitivity.read_text(encoding='utf-8')) if args.copy_sensitivity else None
env=dict(copy_sensitivity_sha256=sha(args.copy_sensitivity) if args.copy_sensitivity else None,hardware_input_sha256=sha(args.hardware_info) if args.hardware_info else None,hardware=hardware,timestamp=datetime.datetime.now().astimezone().isoformat(),platform=platform.platform(),machine=platform.machine(),build=build,binary_sha256=sha(B),source_commit=commit,source_worktree_dirty=dirty,source_sha256=sources,dependencies=deps,settings=cli,profile_sha256=sha(args.profile),measurement_kind='fixed_matched_parameters_normal_vs_single_block_session')
if args.resume:
    previous=json.loads((ROOT/'environment.json').read_text(encoding='utf-8'))
    for key in ('binary_sha256','source_sha256','settings','profile_sha256','hardware_input_sha256','copy_sensitivity_sha256'):
        if previous[key]!=env[key]: raise ValueError('Cannot resume changed '+key)
    env=previous
else:
    (ROOT/'profile-input.json').write_text(args.profile.read_text(encoding='utf-8'),encoding='utf-8')
    if args.hardware_info: shutil.copy2(args.hardware_info, ROOT/'hardware-input.json')
    if args.copy_sensitivity: shutil.copy2(args.copy_sensitivity, ROOT/'copy-sensitivity.json')
    save(ROOT/'environment.json',env); (ROOT/'power-before.txt').write_text(power())
    with tarfile.open(ROOT/'source.tar.gz','w:gz') as archive:
        for path in files: archive.add(path,arcname=str(path.relative_to(R)))
    build_dir=B.parent.parent if B.parent.name in ('Release','Debug') else B.parent
    for name in ('CMakeCache.txt','compile_commands.json'):
        if (build_dir/name).exists(): shutil.copy2(build_dir/name,ROOT/name)
    save(ROOT/'invocation.json',dict(command=[sys.executable,*sys.argv],cwd=str(Path.cwd()),profile_sha256=sha(args.profile)))
    archive_root=R/'docs/performance_archive' if args.report_dir==(R/'docs/reports/comparison').resolve() else args.report_dir/'performance_archive'
    archive_dir=archive_root/('before_'+STAMP);archive_dir.mkdir(parents=True)
    for name in [f'performance_{cache}m_'+('callback' if cache==256 else 'zero')+('_session' if mode=='session' else '')+'.html' for cache in args.cache for mode in write_modes]:
        if (args.report_dir/name).exists(): shutil.copy2(args.report_dir/name,archive_dir/name)
rows=[json.loads(line) for line in (ROOT/'raw.jsonl').read_text(encoding='utf-8').splitlines()] if args.resume and (ROOT/'raw.jsonl').exists() else []
started=time.monotonic();log=(ROOT/'raw.jsonl').open('a',encoding='utf-8');cmdlog=(ROOT/'commands.jsonl').open('a',encoding='utf-8')
counts={};checksums={}
for row in rows:
    r=row['result'];c=r['config'];cache=row['cache_mib']
    key=(cache,c['payload_bytes'],c['callback_iterations'],r['effective_warmup_messages'])
    if key in checksums and checksums[key]!=r['callback_work_checksum']: raise ValueError('Callback checksum differs')
    checksums[key]=r['callback_work_checksum'];counts[cache]=counts.get(cache,0)+1
def run(cache,label,c,mode,trial,stage):
 c=dict(c,mode=mode,messages=args.messages,random_seed=42+trial,verify_payload=stage=='validation')
 for prefix in ('source','output'):assert c[prefix+'_block_size']*c[prefix+'_pool_min']==c[prefix+'_block_size']*c[prefix+'_pool_max']==cache*1048576
 existing=next((x for x in rows if (x["cache_mib"],x["label"],x["stage"],x["trial"],x["result"]["config"]["mode"])==(cache,label,stage,trial,mode)),None)
 if existing:
  if existing["result"]["config"]!=c: raise ValueError("resume configuration mismatch")
  return existing["result"]
 command=[str(B)]
 for k,v in c.items():command += ['--'+k,str(v).lower() if isinstance(v,bool) else str(v)]
 cmdlog.write(json.dumps(dict(cache_mib=cache,label=label,stage=stage,trial=trial,command=command))+'\n');cmdlog.flush()
 proc=subprocess.run(command,capture_output=True,text=True,timeout=c['timeout_seconds']+15)
 if proc.returncode:
  save(ROOT/'failure.json',dict(label=label,command=command,exit_code=proc.returncode,stdout=proc.stdout,stderr=proc.stderr));raise RuntimeError(proc.stderr)
 r=json.loads(proc.stdout)
 assert r['config']==c,(label,set(r['config'])^set(c))
 C=cache*1048576; n=c['payload_bytes']
 if c['transport']=='two_buf_queue':
  retained=2*C
  assert r['nominal_buffer_bytes']==retained
 elif c['transport']=='readerwriterqueue':
  assert r['actual_spsc_capacity']%511==0
  retained=(r['actual_spsc_capacity']//511)*512*n
 elif c['transport']=='rigtorp': retained=(r['actual_spsc_capacity']+1+2*((64+n-1)//n))*n
 else: retained=r['actual_spsc_capacity']*n
 assert retained<=(2*C if c['transport']=='two_buf_queue' else C)
 assert r['consumed']==r['timed_callback_messages']==c['messages']==args.messages
 assert r['timing_scope']=='producer_rounds' and r['seconds']==r['producer_seconds']
 if c['producer_qos']=='user_interactive': assert r['producer_qos_applied']
 if c['producer_cpu']>=0: assert r['producer_hard_affinity']
 else: assert not r['producer_hard_affinity']
 if mode=='write_latency':assert r['latency_samples']*c['latency_batch_size']+r['latency_tail_writes']==args.messages and not r['migrated_samples']
 if c['use_write_session']:assert r['session_block_exhaustions']>=0
 else:assert r['session_block_exhaustions']==0
 key=(cache,c['payload_bytes'],c['callback_iterations'],r['effective_warmup_messages'])
 if key in checksums:assert checksums[key]==r['callback_work_checksum']
 checksums[key]=r['callback_work_checksum']
 row=dict(cache_mib=cache,label=label,stage=stage,trial=trial,command=command,retained_payload_storage_bytes=retained,result=r);rows.append(row);log.write(json.dumps(row)+'\n');log.flush()
 counts[cache]=counts.get(cache,0)+1
 if stage=='callback_calibration' or counts[cache]%10==0:
  print(f'{cache} MiB: {counts[cache]} runs; {stage} {label} {mode}: {r["m_messages_per_second"]:.1f} M/s, ratio={r["callback_to_producer_ratio"]:.2f}, elapsed={time.monotonic()-started:.0f}s',flush=True)
 return r
peers=[dict(file='performance_256m_callback.html',label='256M 普通'),dict(file='performance_256m_callback_session.html',label='256M Session'),dict(file='performance_8m_zero.html',label='8M 普通'),dict(file='performance_8m_zero_session.html',label='8M Session')]
for cache in args.cache:
 policy='zero' if cache==8 else ('fixed' if fixed_loops is not None else 'calibrated')
 specs={label:dict(c) for label,c in settings['caches'][str(cache)].items() if c['payload_bytes'] in args.payload and (c['transport']!='two_buf_queue' or ('session' if c['use_write_session'] else 'normal') in write_modes)}
 matched=args.write_mode=='both' and all({k:v for k,v in specs[f'two_buf_queue_{n}'].items() if k!='use_write_session'}=={k:v for k,v in specs[f'session_{n}'].items() if k!='use_write_session'} for n in args.payload)
 for c in specs.values():
  c.update(messages=args.messages,producer_cpu=args.producer_cpu,producer_qos=args.producer_qos,timer=args.timer,callback_iterations=0)
  assert c['prepared_input'] and c['variable_payload']
  if c['use_write_session']: assert not c['is_scan_current'] and c['publish_count']==4294967295
 calibrations=[]
 for n in args.payload:
  iterations=fixed_loops[n] if policy=='fixed' else 0
  if policy=='calibrated':
   observations={};reference=dict(settings['caches'][str(cache)][f'two_buf_queue_{n}'],messages=args.messages,producer_cpu=args.producer_cpu,producer_qos=args.producer_qos,timer=args.timer,callback_iterations=0);cal_settings=dict(target_ratio=2.5,min_ratio=2,max_ratio=3,repetitions=args.repetitions,messages=args.messages,min_rounds=0,max_iterations=1024,max_candidates=8,reference_publish_count=reference['publish_count'])
   def probe(it):
    vv=[run(cache,f'calibration_{n}_{it}',dict(reference,callback_iterations=it),'throughput',t,'callback_calibration') for t in range(args.repetitions)]
    ratio=statistics.fmean(x['callback_to_producer_ratio'] for x in vv)
    observations[it]=dict(iterations=it,ratio=ratio,ratios=[x['callback_to_producer_ratio'] for x in vv]);return ratio
   iterations=tune.choose_callback_iterations(probe,cal_settings)
   ratio=observations[iterations]['ratio']
   calibrations.append(dict(payload_bytes=n,reference_transport='two_buf_queue',reference_parameters=dict(reference),selected_iterations=iterations,selected_ratio=ratio,target_met=2<=ratio<=3,observations=list(observations.values()),settings=cal_settings))
   print(f'CALIBRATED {n} B: loops={iterations}, ordinary ratio={ratio:.2f}',flush=True)
  for c in specs.values():
   if c['payload_bytes']==n:c['callback_iterations']=iterations
 save(ROOT/f'configs_{cache}m.json',specs);save(ROOT/f'calibration_{cache}m.json',calibrations)
 for label,c in specs.items():run(cache,label,c,'throughput',0,'validation')
 for trial in range(args.repetitions):
  jobs=[(label,mode) for label in specs for mode in ('throughput','write_latency')];random.Random(1701+trial+cache).shuffle(jobs)
  for label,mode in jobs:run(cache,label,specs[label],mode,trial,'final')
 def group(label):
  cc=specs[label];tt=[x['result'] for x in rows if x['cache_mib']==cache and x['label']==label and x['stage']=='final' and x['result']['config']['mode']=='throughput'];ll=[x['result'] for x in rows if x['cache_mib']==cache and x['label']==label and x['stage']=='final' and x['result']['config']['mode']=='write_latency']
  assert len(tt)==len(ll)==args.repetitions
  avg=lambda k:statistics.fmean(x[k] for x in tt)
  g=dict(payload_bytes=cc['payload_bytes'],transport=cc['transport'],callback_iterations=cc['callback_iterations'],parameters=cc,throughput_mps=avg('m_messages_per_second'),throughput_runs_mps=[x['m_messages_per_second'] for x in tt],payload_gb_per_second=avg('payload_gb_per_second'))
  for q in ('p50','p99','p999'):g[q+'_ns']=statistics.fmean(x['batch_average_write_ns'][q] for x in ll);g[q+'_runs_ns']=[x['batch_average_write_ns'][q] for x in ll]
  g['latency_runs']=[x['batch_average_write_ns'] for x in ll]
  for k in ('callback_to_producer_ratio','producer_seconds','callback_processing_seconds','wall_seconds','drain_seconds','failed_write_calls','backpressured_messages','producer_retry_seconds','cpu_cores','input_preparation_seconds','session_block_exhaustions'):g[k]=avg(k)
  for k in ('actual_spsc_capacity','completed_rounds','effective_warmup_messages','prepared_input_bytes'):g[k]=sorted({x[k] for x in tt+ll})
  for k in ('latency_samples','latency_tail_writes','timer'):g[k]=sorted({x[k] for x in ll})
  for k in ('writer_pressure_events','copy_memory_full_events'):g[k]=statistics.fmean((x['library_backpressure'] or {}).get(k,0) for x in tt)
  g['internal_backpressure']=dict(writer_pressure_events=g['writer_pressure_events'],copy_memory_full_events=g['copy_memory_full_events']) if all(x['library_backpressure'] is not None for x in tt) else None
  g['nominal_buffer_mib']=sorted({x['nominal_buffer_bytes']/1048576 for x in tt+ll});g['peak_rss_mib']=max(x['peak_rss_mib'] for x in tt+ll);g['latency_quantum_ns']=max(x['latency_quantum_ns'] for x in ll);g['resolution_limited']=any(x['resolution_limited'] for x in ll);g['p999_sample_count_warning']=any(x['p999_sample_count_warning'] for x in ll);g['library_pressure_stats_available']=all(x['library_backpressure'] is not None for x in tt)
  return g
 for mode in write_modes:
  directory=ROOT/f'{cache}m_{mode}';directory.mkdir(exist_ok=True)
  selected=[(f'session_{n}' if mode=='session' else f'two_buf_queue_{n}') if t=='two_buf_queue' else f'{t}_{n}' for n in args.payload for t in ('two_buf_queue','readerwriterqueue','rigtorp','atomic_queue')]
  rr=[x for x in rows if x['cache_mib']==cache and x['label'] in selected and x['stage'] in ('validation','final')]
  stem='performance_256m_callback' if cache==256 else 'performance_8m_zero';html_name=stem+('_session' if mode=='session' else '')+'.html'
  tbq_configs=[specs[f'session_{n}' if mode=='session' else f'two_buf_queue_{n}'] for n in args.payload]
  publish_counts=sorted({c['publish_count'] for c in tbq_configs})
  profile=dict(cache_mib=cache,callback_policy=policy,payload_bytes=args.payload,messages=args.messages,repetitions=args.repetitions,publish_count=publish_counts[0] if len(publish_counts)==1 else publish_counts,write_mode=mode,report_peers=peers,block_note='按长度保留冻结的块大小和每层容量。普通 write 与 Session 的读端及发布配置各自独立，完整参数如下。')
  notes=['本次没有重新寻优，吞吐和各延时指标使用同一构造配置。'+('普通 write 与 Session 使用相同参数。' if matched else '普通 write 与 Session 的读端/发布配置和测量轮次可能不同，不能当作同参数对照。'),('同缓存的两份报告共享本次实测的开源对照数据；没有使用历史成绩。' if args.write_mode=='both' else '只重新测量 '+mode+' 与三个开源对照；另一写模式的报告保持原样，没有把它的历史数据混入本轮主表。'),'每层数据池严格为 '+str(cache)+' MiB；原生队列的保留槽与 rigtorp 首尾 padding 纳入这个 payload 存储预算，队列元数据和输入另计。','Session 正常遇到块满返回 insufficient_space，次数记入 session_block_exhaustions；这不是背压或丢消息。池/队列受压通过 Writer 内部压力统计报告。']
  notes+=['rigtorp_write_prefetch 在完整参数中记录；开关在测试开始前选择模板，普通复制和带提示构造都只复制一次完整负载，不额外写零。atomic_queue 保持原有调用。']
  notes+=['ReaderWriterQueue 16/32 B 使用 copy_after_zero：队列槽位默认清零后 memcpy 一次完整负载；8/64 B 保留成员复制。清零和复制均在写入计时内。各实现的调用端优化在行标签和完整参数中公开，不能把所有差异归因于队列算法。']
  if mode=='session':notes+=['get_current_session → 内层逐条写 → 空间不足退出 → 外层 get_next_session。获取、等待、换块与 Session 状态归还包含在写入计时中。分组延时跨块继续计时，没有在块边界提前结束计时组。','不主动 flush；flush 只发布位置，不切换 block。每轮尾部在计时外 finish_block，最后一轮 finish，关闭 current 扫描也能收齐尾部。']
  if policy=='calibrated':notes+=['回调循环以普通 write 校准后固定，各实现共享同一长度的循环次数；实际回调/写入时间比可以不同。']
  if policy=='fixed':notes+=['256 MiB 回调循环按 8/16/32/64 B 固定为 '+ '/'.join(str(fixed_loops[n]) for n in (8,16,32,64))+' 次，本次未重新校准。用于控制读端/发布参数变化；实际时间比保留在表格中，不保证仍在 2～3 倍范围。']
  for c in tbq_configs:
   notes+=['TwoBufQueue '+str(c['payload_bytes'])+' B：is_scan_current='+str(c['is_scan_current']).lower()+', publish_count='+str(c['publish_count'])+', 初始/最大扫描间隔='+str(c['rescan_interval_ns'])+'/'+str(c['rescan_max_interval_ns'])+' ns，最小等待='+str(c['work_wait_min_ms'])+' ms。']
  summary=dict(schema_version=2,profile=profile,environment=env,groups=[group(x) for x in selected],calibration=calibrations,search=None,formal_runs=sum(x['stage']=='final' for x in rr),validation_runs=sum(x['stage']=='validation' for x in rr),calibration_runs=sum(x['cache_mib']==cache and x['stage']=='callback_calibration' for x in rows),measurement_kind='matched_parameters' if matched else 'fixed_parameters_independent_write_mode',audit_notes=notes)
  summary['copy_sensitivity']=dict(copy_sensitivity,source_prefix=os.path.relpath(args.copy_sensitivity.resolve().parent,args.report_dir).replace(os.sep,"/")) if copy_sensitivity else None
  save(directory/'results.json',summary);save(directory/'profile.json',profile);save(directory/'environment.json',env);save(directory/'dependencies.json',deps);
  save(directory/'reproduction.json',dict(source_archive='../source.tar.gz',invocation='../invocation.json',commands='../commands.jsonl',compile_commands='../compile_commands.json')); save(directory/'callback-calibration.json',calibrations)
  (directory/'raw.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in rr))
  save(directory/'audit.json',dict(messages=args.messages,repetitions=args.repetitions,full_payload_fifo_validated=True,successful_timed_writes=args.messages,block_get_and_switch_timed=True,session_block_exhaustions_are_not_backpressure=True,native_controls_shared=args.write_mode=='both',binary_sha256=sha(B),source_sha256=sources,parameters_matched=matched,runtime_cache_bounds_verified=True))
  write_html_report(summary,args.report_dir/html_name,directory)
  print('REPORT',html_name,flush=True)
  for g in summary['groups']:
   if g['transport']=='two_buf_queue':print(mode,g['payload_bytes'],*(round(g[k],3) for k in ('throughput_mps','p50_ns','p99_ns','p999_ns')),flush=True)
 log.flush()
(ROOT/'power-after.txt').write_text(power());save(ROOT/'manifest.json',dict(timestamp=STAMP,binary_sha256=sha(B),source_sha256=sources,environment=env,elapsed_seconds_this_invocation=time.monotonic()-started,resumed=args.resume,runs=len(rows),counts=counts,policy='All trials retained; write modes configured independently; original upstream headers'))
log.close();cmdlog.close();print('COMPLETE',ROOT,flush=True)
