from pathlib import Path
import datetime, hashlib, json, random, statistics, subprocess, sys

root = Path(__file__).resolve().parent
configs = json.loads((root / sys.argv[1]).read_text())
repetitions = int(sys.argv[2]) if len(sys.argv) > 2 else 5
destination = root / (Path(sys.argv[1]).stem + '-' + datetime.datetime.now().strftime('%H%M%S'))
destination.mkdir()
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
manifest = dict(configurations=configs, repetitions=repetitions,
                binaries={v['binary']: sha(root/v['binary']) for v in configs.values()},
                power=subprocess.check_output(['pmset','-g','batt'],text=True))
(destination/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
rows=[]
with (destination/'raw.jsonl').open('w') as raw:
    def run(label,trial,validation):
        spec=configs[label]
        c=dict(spec['config'],mode='write_latency',verify_payload=validation,random_seed=42+trial)
        command=[str(root/spec['binary'])]
        for key,value in c.items(): command.extend(('--'+key,str(value).lower() if isinstance(value,bool) else str(value)))
        p=subprocess.run(command,capture_output=True,text=True,timeout=c['timeout_seconds']+15)
        if p.returncode: raise RuntimeError(p.stderr)
        r=json.loads(p.stdout)
        assert r['config']==c and r['consumed']==r['timed_callback_messages']==c['messages']==10000000
        assert not r['failed_write_calls'] and not (r['library_backpressure'] or {}).get('writer_blocked_events',0)
        assert r['latency_samples']*1000+r['latency_tail_writes']==c['messages']
        assert r['producer_qos_applied'] and not r['producer_hard_affinity']
        row=dict(label=label,trial=trial,validation=validation,result=r)
        rows.append(row);raw.write(json.dumps(row)+'\n');raw.flush()
    for label in configs: run(label,0,True)
    jobs=[(label,trial) for label in configs for trial in range(repetitions)]
    random.Random(45127).shuffle(jobs)
    for label,trial in jobs: run(label,trial,False)
summary={}
for label in configs:
    matching=[r['result'] for r in rows if r['label']==label and not r['validation']]
    summary[label]={metric:statistics.fmean(r['batch_average_write_ns'][metric] for r in matching) for metric in ('p50','p99','p999')}
    summary[label]['runs']=[r['batch_average_write_ns']['p50'] for r in matching]
    summary[label]['writer_pressure_events']=[(r['library_backpressure'] or {}).get('writer_pressure_events') for r in matching]
    summary[label]['write_seconds']=statistics.fmean(r['producer_seconds'] for r in matching)
(destination/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
print('SAVED',destination,flush=True)
for label,value in summary.items(): print(label,value,flush=True)
