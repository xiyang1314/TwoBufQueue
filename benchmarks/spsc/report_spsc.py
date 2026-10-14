#!/usr/bin/env python3
"""Run a fixed unframed SPSC comparison and save audited JSON plus an offline HTML report."""
import argparse, datetime, hashlib, json, platform, random, statistics, subprocess, time
from pathlib import Path
import tune_spsc as tune

TRANSPORTS = ('two_buf_queue','readerwriterqueue','rigtorp','atomic_queue')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def mean(rows,key):
    return statistics.fmean(r[key] for r in rows)

def run_comparison(executable,profile,output):
    output.mkdir(parents=True,exist_ok=False)
    executable=executable.resolve()
    defaults=json.loads(subprocess.check_output([str(executable),'--defaults'],text=True))
    build=json.loads(subprocess.check_output([str(executable),'--build-info'],text=True))
    if not build['optimized'] or build['sanitized']:
        raise ValueError('the performance report requires an optimized, unsanitized binary')
    lengths=profile['payload_bytes'];repetitions=profile['repetitions'];messages=profile['messages']
    if not lengths or any(n not in (8,16,32,64) for n in lengths) or len(set(lengths))!=len(lengths):
        raise ValueError('payload_bytes must contain distinct values from 8/16/32/64')
    if repetitions<1 or messages<1000 or profile['callback_policy'] not in ('zero','calibrated'):
        raise ValueError('invalid repetition/message/callback configuration')
    base=dict(defaults,**profile['base'])
    base['publish_count']=profile.get('publish_count',4)
    cache_bytes=profile['cache_mib']*1024*1024
    assert base['source_block_size']*base['source_pool_min']==cache_bytes
    assert base['source_block_size']*base['source_pool_max']==cache_bytes
    assert base['output_block_size']*base['output_pool_min']==cache_bytes
    assert base['output_block_size']*base['output_pool_max']==cache_bytes
    assert base['spsc_buffer_bytes']==cache_bytes
    assert 'flush_every' not in base and base['output_mode']=='spsc'
    started=time.monotonic();rows=[];checksums={};calibrations=[];loops_by_length={}
    environment=dict(timestamp=datetime.datetime.now().astimezone().isoformat(),
        platform=platform.platform(),machine=platform.machine(),build=build,
        executable=str(executable),binary_sha256=sha(executable),
        comparison='fixed-size unframed TwoBufQueue and three unmodified upstream SPSC queues',
        aggregation='arithmetic mean of per-run throughput and per-run quantiles; no pooled quantiles',
        latency=f'quantiles of group average elapsed time for {base["latency_batch_size"]} successful writes, including retries and loop overhead',
        callback_time='first-to-last callback interval within each round, including dispatch and in-round waits; excluding warmup and between-round idle')
    def power():
        if platform.system()!='Darwin':return 'power status not collected on this platform'
        return subprocess.check_output(['pmset','-g','batt'],text=True)
    (output/'power-before.txt').write_text(power())
    (output/'profile.json').write_text(json.dumps(profile,indent=2)+'\n')
    (output/'environment.json').write_text(json.dumps(environment,indent=2)+'\n')
    (output/'defaults.json').write_text(json.dumps(defaults,indent=2)+'\n')
    dependencies=json.loads(Path(__file__).with_name('spsc_dependencies.json').read_text())
    (output/'dependencies.json').write_text(json.dumps(dependencies,indent=2)+'\n')
    with (output/'raw.jsonl').open('w') as raw:
        def run(length,transport,mode,trial,stage,iterations):
            config=dict(base,payload_bytes=length,transport=transport,mode=mode,
                messages=profile.get('validation_messages',20000) if stage=='validation' else messages,
                callback_iterations=iterations,verify_payload=stage=='validation',random_seed=42+trial)
            if stage=='validation':config['warmup_messages']=min(base['warmup_messages'],config['messages'])
            command=[str(executable)]
            for key,value in config.items():
                command.extend(['--'+key,str(value).lower() if isinstance(value,bool) else str(value)])
            process=subprocess.run(command,capture_output=True,text=True,timeout=config['timeout_seconds']+10)
            if process.returncode:
                (output/'failure.json').write_text(json.dumps(dict(command=command,stderr=process.stderr),indent=2)+'\n')
                raise RuntimeError(process.stderr)
            r=json.loads(process.stdout)
            assert r['source_record_bytes']==length and r['config']['callback_iterations']==iterations
            assert r['consumed']==r['timed_callback_messages']==config['messages']
            assert r['completed_rounds']==tune.expected_round_count(config,r)
            assert r['seconds']==r['producer_seconds'] and r['timing_scope']=='producer_rounds'
            if config['producer_qos']=='user_interactive':assert r['producer_qos_applied']
            assert r['producer_hard_affinity']==(config['producer_cpu']>=0)
            signature=(length,iterations,r['effective_warmup_messages'],config['messages'])
            if signature in checksums:assert checksums[signature]==r['callback_work_checksum'],'callback checksum mismatch'
            checksums[signature]=r['callback_work_checksum']
            if not iterations:assert r['callback_work_checksum']==0
            if mode=='write_latency':
                assert r['latency_samples']*config['latency_batch_size']+r['latency_tail_writes']==config['messages']
                assert not r['migrated_samples']
            row=dict(stage=stage,trial=trial,payload_bytes=length,transport=transport,command=command,result=r)
            raw.write(json.dumps(row)+'\n');raw.flush();rows.append(row)
            print(f"{profile['cache_mib']} MiB {stage} {length} B {transport} {mode} #{trial+1} loops={iterations} {r['m_messages_per_second']:.1f} M/s ratio={r['callback_to_producer_ratio']:.2f}x ({time.monotonic()-started:.1f}s)",flush=True)
            return r
        for length in lengths:
            if profile['callback_policy']=='zero':
                iterations=0
            else:
                settings=dict(profile['callback_calibration'],messages=messages,repetitions=repetitions,
                    reference_publish_count=base['publish_count'],min_rounds=1)
                tune.validate_calibration(settings);observations={}
                def probe(n):
                    measured=[run(length,'two_buf_queue','throughput',trial,'callback_calibration',n) for trial in range(repetitions)]
                    record=dict(iterations=n,ratio=mean(measured,'callback_to_producer_ratio'),
                        ratios=[r['callback_to_producer_ratio'] for r in measured],
                        producer_seconds=mean(measured,'producer_seconds'),
                        callback_processing_seconds=mean(measured,'callback_processing_seconds'))
                    observations[n]=record
                    print(f"Calibration {length} B: {n} loops -> {record['ratio']:.3f}x",flush=True)
                    return record['ratio']
                iterations=tune.choose_callback_iterations(probe,settings)
                calibrations.append(dict(payload_bytes=length,reference_transport='two_buf_queue',
                    selected_iterations=iterations,selected_ratio=observations[iterations]['ratio'],
                    target_met=settings['min_ratio']<=observations[iterations]['ratio']<=settings['max_ratio'],
                    observations=list(observations.values()),settings=settings))
                (output/'callback-calibration.json').write_text(json.dumps(calibrations,indent=2)+'\n')
            loops_by_length[length]=iterations
            for transport in TRANSPORTS:
                for mode in ('throughput','write_latency'):run(length,transport,mode,0,'validation',iterations)
            rng=random.Random(98137+length)
            for trial in range(repetitions):
                jobs=[(transport,mode) for transport in TRANSPORTS for mode in ('throughput','write_latency')]
                rng.shuffle(jobs)
                for transport,mode in jobs:run(length,transport,mode,trial,'formal',iterations)
    groups=[]
    for length in lengths:
        for transport in TRANSPORTS:
            selected=[row['result'] for row in rows if row['stage']=='formal' and row['payload_bytes']==length and row['transport']==transport]
            through=[r for r in selected if r['config']['mode']=='throughput']
            latency=[r for r in selected if r['config']['mode']=='write_latency']
            assert len(through)==len(latency)==repetitions
            pressure=[r['library_backpressure'] for r in through]
            internal={k:statistics.fmean(p[k] for p in pressure) for k in ('writer_pressure_events','writer_rejected_attempts','writer_blocked_events','copy_memory_full_events','copy_backpressured_frames')} if all(p is not None for p in pressure) else None
            groups.append(dict(payload_bytes=length,transport=transport,callback_iterations=loops_by_length[length],
                throughput_mps=mean(through,'m_messages_per_second'),payload_gb_per_second=mean(through,'payload_gb_per_second'),
                p50_ns=statistics.fmean(r['batch_average_write_ns']['p50'] for r in latency),
                p99_ns=statistics.fmean(r['batch_average_write_ns']['p99'] for r in latency),
                p999_ns=statistics.fmean(r['batch_average_write_ns']['p999'] for r in latency),
                throughput_runs_mps=[r['m_messages_per_second'] for r in through],
                latency_runs=[r['batch_average_write_ns'] for r in latency],
                callback_to_producer_ratio=mean(through,'callback_to_producer_ratio'),
                producer_seconds=mean(through,'producer_seconds'),callback_processing_seconds=mean(through,'callback_processing_seconds'),
                wall_seconds=mean(through,'wall_seconds'),drain_seconds=mean(through,'drain_seconds'),
                failed_write_calls=mean(through,'failed_write_calls'),backpressured_messages=mean(through,'backpressured_messages'),
                producer_retry_seconds=mean(through,'producer_retry_seconds'),internal_backpressure=internal,
                actual_spsc_capacity=sorted({r['actual_spsc_capacity'] for r in selected}),
                nominal_buffer_mib=sorted({r['nominal_buffer_bytes']/1048576 for r in selected}),
                peak_rss_mib=max(r['peak_rss_mib'] for r in selected),cpu_cores=mean(through,'cpu_cores'),
                completed_rounds=sorted({r['completed_rounds'] for r in selected}),
                effective_warmup_messages=sorted({r['effective_warmup_messages'] for r in selected}),
                latency_samples=sorted({r['latency_samples'] for r in latency}),
                latency_tail_writes=sorted({r['latency_tail_writes'] for r in latency}),
                p999_sample_count_warning=any(r['p999_sample_count_warning'] for r in latency),
                timer=sorted({r['timer'] for r in latency}),latency_quantum_ns=mean(latency,'latency_quantum_ns'),
                resolution_limited=any(r['resolution_limited'] for r in latency),parameters=through[0]['config']))
    summary=dict(schema_version=1,profile=profile,environment=environment,groups=groups,
        calibration=calibrations,formal_runs=sum(row['stage']=='formal' for row in rows),
        validation_runs=sum(row['stage']=='validation' for row in rows),
        calibration_runs=sum(row['stage']=='callback_calibration' for row in rows),elapsed_seconds=time.monotonic()-started)
    audit=dict(all_message_counts_and_fifo_verified=True,callback_checksums_match=True,source_record_bytes_equal_payload=True,
        no_active_flush=True,one_unframed_two_buf_queue_only=True,binary_sha256=sha(executable),
        zero_failed_writes=all(row['result']['failed_write_calls']==0 for row in rows if row['stage']=='formal'),
        formal_runs=summary['formal_runs'],validation_runs=summary['validation_runs'],calibration_runs=summary['calibration_runs'])
    assert audit['binary_sha256']==environment['binary_sha256']
    (output/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
    (output/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    (output/'power-after.txt').write_text(power())
    return summary

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable',type=Path);parser.add_argument('--profile',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path);parser.add_argument('--html',required=True,type=Path)
    args=parser.parse_args()
    profile=json.loads(args.profile.read_text())
    summary=run_comparison(args.executable,profile,args.output)
    from spsc_html_report import write_html_report
    write_html_report(summary,args.html,args.output)
    print('HTML report:',args.html.resolve(),flush=True)

if __name__=='__main__':main()
