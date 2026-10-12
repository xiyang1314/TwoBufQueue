from pathlib import Path
import sys,json,statistics
repo=Path('/Users/lc/Documents/TwoBufQueue');sys.path.insert(0,str(repo/'benchmarks'));import run_burst_showcase as p
root=Path(__file__).resolve().parent;profile=json.loads((root/'frozen_profile.json').read_text());folder=root/'retry_cpu_control'
cases=[('whole_burst_yield','two_buf_queue',root/'build-pmu/test_burst_cache','TwoBufQueue',{}),('consecutive_yield','two_buf_queue',root/'build-consecutive/test_burst_cache','TwoBufQueue',{}),('no_yield','two_buf_queue',root/'build-pmu/test_burst_cache','TwoBufQueue',{'retry_yield_every':0}),('ReaderWriterQueue','readerwriterqueue',root/'build-consecutive/test_burst_cache','ReaderWriterQueue',{}),('rigtorp','rigtorp',root/'build-consecutive/test_burst_cache','rigtorp',{}),('atomic_queue','atomic_queue',root/'build-consecutive/test_burst_cache','atomic_queue',{})]
manifest={'scope':'1MiB whole-burst versus per-message consecutive retry;wall mode0;no counters around business;CPU snapshots outside timing','profile':profile,'records':[]}
for run in range(1,4):
 offset=(run-1)*2%len(cases)
 for name,transport,exe,key,overrides in cases[offset:]+cases[:offset]:
  parameters=dict(profile['queue_parameters'][key]);parameters.update(overrides)
  raw=p.run_case(exe,folder,48,(name,transport,{'probe_mode':0}),run,[1],2500,64,900,True,progress=512,queue_parameters=parameters,benchmark_parameters=profile['benchmark_parameters']);p.validate(raw,48,[1],2500,64)
  samples=[v for pt in raw['points'] for v in pt['after_business_ns']];top=sorted([(v,pt) for pt in raw['points'] for v in pt['after_business_ns']],key=lambda x:x[0])[-100:]
  row={'case':name,'run':run,**{k:p.nearest(samples,t) for k,t in [('p50',.5),('p99',.99),('p999',.999)]},'cpu_changed_during_write_windows':sum(pt['warm_write_cpus'][1]!=pt['warm_write_cpus'][2] for pt in raw['points']),'slowest100groups_cpu_changed':sum(pt['warm_write_cpus'][1]!=pt['warm_write_cpus'][2] for _,pt in top),'writer_us_per_burst':statistics.mean(pt['writer_ns'] for pt in raw['points'])/1000,'retry_total':sum(pt['retries'] for pt in raw['points']),'checksum':raw['business_checksum']}
  manifest['records'].append(row);p.save(folder/'manifest.json',manifest);print('RESULT',json.dumps(row),flush=True)
rows=[]
for name,_,_,_,_ in cases:
 rr=[r for r in manifest['records'] if r['case']==name];rows.append({'case':name,**{k:statistics.mean(r[k] for r in rr) for k in ['p50','p99','p999','cpu_changed_during_write_windows','writer_us_per_burst']},'per_run':rr})
assert len({r['checksum'] for r in manifest['records']})==1
p.save(folder/'summary.json',rows)
