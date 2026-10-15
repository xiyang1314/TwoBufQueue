#!/usr/bin/env python3
"""Repeat the 1 MiB tail experiment: fixed workload, three retry strategies,
three regular open-source queues, 10,000 immediate-post groups per run, 3 runs.
"""
import argparse,json,pathlib,statistics,sys,time
from types import SimpleNamespace
import run_burst_showcase as pipeline
import tune_burst_business as tuning
from burst_tail_report import render
ROOT=pathlib.Path(__file__).resolve().parents[2]


def execute(executable,folder,profile,resume=False):
    variants=[('TwoBufQueue_yield1024','two_buf_queue',1024),
              ('TwoBufQueue_yield0','two_buf_queue',0),
              ('TwoBufQueue_yield65536','two_buf_queue',65536),
              ('ReaderWriterQueue','readerwriterqueue',1024),
              ('rigtorp','rigtorp',1024),('atomic_queue','atomic_queue',1024)]
    folder.mkdir(parents=True,exist_ok=True)
    saved_profile=folder/'selected_config.json'
    if saved_profile.exists() and json.loads(saved_profile.read_text())!=profile:raise ValueError('saved profile changed')
    pipeline.save(saved_profile,profile)
    manifest={'schema':'burst_tail_1m_v1','parameters':profile,'runs':3,
              'samples_per_run':10000,'source_total_bytes':16384,
              'output_total_bytes':256*1048576,'burst_bytes':1048576,
              'executable_sha256':tuning.sha(executable),'records':[]}
    rows=[]
    for run in range(1,4):
        offset=(run-1)*2
        for label,transport,interval in variants[offset:]+variants[:offset]:
            base='TwoBufQueue' if transport=='two_buf_queue' else label
            q=dict(profile['queue_parameters'][base]);q['retry_yield_every']=interval
            raw=pipeline.run_case(executable,folder,profile['selected_business_kib'],
                (label,transport,{}),run,[1],2500,profile['callback_iterations'],
                profile['process_timeout'],resume,progress=512,queue_parameters=q,
                benchmark_parameters=profile['benchmark_parameters'])
            pipeline.validate(raw,profile['selected_business_kib'],[1],2500,profile['callback_iterations'])
            groups=[x for point in raw['points'] for x in point['after_business_ns']]
            row={'queue':label,'run':run,'samples':len(groups),
                'before_ns':statistics.mean(point['pre_business_ns'] for point in raw['points']),
                **{key:pipeline.nearest(groups,p) for key,p in [('p50',.5),('p99',.99),('p999',.999)]},
                'write_ms':sum(point['writer_ns'] for point in raw['points'])/1e6,
                'bp_percent':sum(point['backpressured_messages'] for point in raw['points'])/(2500*32768)*100}
            rows.append(row);manifest['records'].append({'queue':label,'run':run,
                'file':f'raw/{profile["selected_business_kib"]}k_{label}_run{run}.json'})
            pipeline.save(folder/'manifest.json',manifest)
    checksums={json.loads((folder/r['file']).read_text())['business_checksum'] for r in manifest['records']}
    if len(checksums)!=1:raise ValueError('business workload differs')
    result={'schema':'burst_tail_1m_result_v1','parameters':profile,'verified':True,
            'rows':[],'business_checksum':checksums.pop()}
    for label,_,_ in variants:
        rr=[x for x in rows if x['queue']==label]
        result['rows'].append({'queue':label,'per_run':rr,
            **{k:statistics.mean(x[k] for x in rr) for k in ('before_ns','p50','p99','p999','write_ms','bp_percent')}})
    pipeline.save(folder/'report_results.json',result)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',type=pathlib.Path,default=ROOT/'benchmarks/profiles/burst_tail_1m.json')
    parser.add_argument('--executable',type=pathlib.Path)
    parser.add_argument('--deps',type=pathlib.Path,default=ROOT/'third_party/spsc')
    parser.add_argument('--build-dir',type=pathlib.Path,default=ROOT/'build-burst-showcase')
    parser.add_argument('--compiler');parser.add_argument('--jobs',type=int,default=4)
    parser.add_argument('--output',type=pathlib.Path,default=ROOT/'docs/performance_data'/('burst_tail_1m_'+time.strftime('%Y%m%d_%H%M%S')))
    parser.add_argument('--report',type=pathlib.Path,default=ROOT/'docs/reports/burst/performance_burst_tail_1m.html')
    parser.add_argument('--resume',action='store_true');parser.add_argument('--render-only',action='store_true')
    args=parser.parse_args()
    if args.render_only:result=json.loads((args.output/'report_results.json').read_text())
    else:
        profile=json.loads(args.profile.read_text());args.compiler=pipeline.choose_compiler(args.compiler,profile)
        args.recorded_build=profile.get('build',{})
        executable=pipeline.build(args);result=execute(executable.resolve(),args.output.resolve(),profile,args.resume)
    render(args.output.resolve(),args.report.resolve(),result)
    print('Saved:',args.report,flush=True)


if __name__=='__main__':main()
