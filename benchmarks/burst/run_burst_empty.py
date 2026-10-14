#!/usr/bin/env python3
"""Replay the backlog workload without preparing backlog; start each burst empty.

One command captures three independent runs, fixed original workload/queue
parameters, raw samples, full validation and a separate HTML report.
"""
import argparse
import hashlib
import html
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time
import run_burst_showcase as base
import tune_burst_business as tuning
from burst_replay import compiler_info, choose_compiler
from burst_showcase_report import comparison_cards, group_style, QUEUES, NAMES

ROOT=Path(__file__).resolve().parents[2]
MIB=1048576


def validate_empty(data, profile):
    base.validate(data,profile['selected_business_kib'],profile['bursts_mib'],profile['final_window_repeats'],profile['callback_iterations'],regular=False)
    if data['prefill'] or data['setup'] or data['accepted']!=data['formal']:
        raise ValueError('unexpected setup messages')
    if not data['drain_before_burst']:
        raise ValueError('empty-before-each-burst mode not enabled')
    for point in data['points']:
        if point['pending_before']!=0 or point['setup_messages']!=0 or point['pending_after']>point['burst_bytes']:
            raise ValueError('empty-start invariant failed')


def summarize(folder, profile):
    rows=[];checksums=set()
    for label,_,_ in tuning.CASES:
        points_by_burst={b:[] for b in profile['bursts_mib']}
        for run in range(1,profile['runs']+1):
            data=json.loads((folder/f'formal/raw/{profile["selected_business_kib"]}k_{label}_run{run}.json').read_text())
            validate_empty(data,profile);checksums.add(data['business_checksum'])
            for burst in profile['bursts_mib']:
                points=[p for p in data['points'] if p['burst_bytes']==burst*MIB]
                samples=[v for p in points for v in p['after_business_ns']]
                if len(samples)<profile['samples_per_run']:raise ValueError('insufficient samples')
                points_by_burst[burst].append({'run':run,'samples':len(samples),
                    'p50':base.nearest(samples,.5),'p99':base.nearest(samples,.99),'p999':base.nearest(samples,.999),
                    'pending_before_max':max(p['pending_before'] for p in points),
                    'pending_after_mean_bytes':statistics.mean(p['pending_after'] for p in points),
                    'pending_after_max_bytes':max(p['pending_after'] for p in points),
                    'backpressured_messages':sum(p['backpressured_messages'] for p in points)})
        for burst,runs in points_by_burst.items():
            rows.append({'queue':label,'burst_mib':burst,
                **{key:statistics.mean(r[key] for r in runs) for key in ('p50','p99','p999')},'per_run':runs})
    if len(checksums)!=1:raise ValueError('business workload differs across runs or queues')
    result={'schema':'burst_empty_result_v1','parameters':profile,'rows':rows,'verified':True,
            'business_checksum':checksums.pop(),'timer_name':data['timer_name'],'timer_quantum_ns':data['timer_quantum_ns']}
    base.save(folder/'formal/report_results.json',result);return result


def render(folder, report, result):
    p=result['parameters'];bursts=p['bursts_mib'];by={(r['queue'],r['burst_mib']):r for r in result['rows']}
    baseline=statistics.mean(by['baseline',b]['p50'] for b in bursts)
    normal_ns=round(baseline)
    if normal_ns<=0:raise ValueError('normal business reference must be positive')
    css=(ROOT/'benchmarks/burst/burst_report_style.css').read_text()
    prefix=os.path.relpath(folder,report.parent).replace(os.sep,'/')
    profile_reference=os.path.relpath(folder/'selected_config.json',ROOT).replace(os.sep,'/')
    title='连续发送一批消息，对业务性能产生什么影响'
    h='<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>'+title+' · TwoBufQueue</title><style>'+css+'</style></head><body><main>'
    h+='<section class="hero"><h1>'+title+'</h1><p>比较几种消息队列在突发连续发送后，对生产者线程业务执行性能的影响。</p><span class="tag">三次独立运行</span><span class="tag">每批从零积压开始</span></section>'
    h+='<section><h2>模拟场景</h2><p>在通常人们使用 SPSC 队列的时候会遇到一个问题，就是时不时会出现消费者线程因为突发原因而产生消息积压，从而拖累生产者线程。这时处理方式一般是加大队列的缓存容量。但是，缓存队列容量要分配多少才能避免这种偶然突发而造成的背压，这是个令人头痛的问题。而且即便扩大缓存容量，突发地持续发送一批消息也会影响生产者线程 L1、L2 或者 L3 的缓存命中率，从而影响热路径性能。这对于比如机器人、高频交易等需要热路径极致低延时的应用来说影响会比较大。而双层缓存能比较好地解决这类问题，这正是 TwoBufQueue 的设计思路。</p><p>这个测试模拟的是突发持续地发送一批消息，让消息队列产生消息积压，看看几种队列在这种情况下对业务产生的性能影响状况。</p></section>'
    h+='<section><h2>测试流程</h2><div class="steps"><span>正常执行业务函数</span><b>→</b><span>循环发送一批消息</span><b>→</b><span>立刻执行业务函数</span></div><p>生产者线程先循环执行业务函数，执行完后循环发送一批消息到消息队列，等发送完再立刻执行同样的业务函数。</p><p>这里统计的是发送完消息后执行业务函数的耗时，以及它相对于正常执行同样业务函数耗时的比值。每批从零积压开始，测量后在计时外排空，再开始下一批；消费者与复制线程持续运行。</p></section>'
    h+=f'<section id="performance-results"><h2>性能测试</h2><p class="baseline">正常执行业务函数耗时：约 {normal_ns} ns/次</p><p>表中耗时为发送后每组 16 次业务调用平均耗时的 <b>p50</b>，再对三轮取平均。<b>比值 = 发送后耗时 ÷ 正常耗时（{normal_ns} ns/次）</b>；数值越低越好。</p>'
    h+=comparison_cards(bursts,by)
    h+='<div class="scroll"><table class="with-ratio" id="performance"><colgroup><col class="volume-col"><col class="queue-col"><col class="cost-col"><col class="ratio-col"></colgroup><thead><tr><th class="volume-heading" scope="col">每批发送量</th><th class="queue-heading" scope="col">队列</th><th class="cost" scope="col">耗时(ns/次)</th><th class="ratio" scope="col">比值</th></tr></thead>'
    for index,burst in enumerate(bursts):
        h+=f'<tbody class="burst-group" style="{group_style(index)}">'
        for pos,queue in enumerate(QUEUES):
            value=by[queue,burst]['p50']
            h+='<tr class="'+('tb' if queue=='TwoBufQueue' else '')+'">'
            if pos==0:h+=f'<th class="volume-label" scope="rowgroup" rowspan="4">{burst} MiB<small>{burst*MIB//32:,} 条消息</small></th>'
            h+=f'<th class="queue" scope="row">{NAMES[queue]}</th><td class="cost"><span class="metric-value">{value:.1f}</span></td><td class="ratio">{value/normal_ns:.2f}×</td></tr>'
        h+='</tbody>'
    h+='</table></div><p class="muted">正常耗时来自同批无队列对照，精确值保留在原始数据中；比值统一使用上方显示的整数参考值。这里测量的是业务执行成本，不是单条消息从发送到接收的延时。</p></section>'
    two_min=min(by['TwoBufQueue',b]['p50'] for b in bursts)
    two_max=max(by['TwoBufQueue',b]['p50'] for b in bursts)
    under_twenty='，最高不到 20 ns/次' if two_max<20 else ''
    h+=f'<section class="report-conclusion"><h2>结论</h2><ul><li>总体上，连续发送的消息量越大，对热路径业务性能的影响越大。</li><li>本次测量中，TwoBufQueue 采用双层缓存设计，业务耗时从正常的约 {normal_ns} ns/次增大到 {two_min:.1f}–{two_max:.1f} ns/次{under_twenty}，对热路径业务的影响较小。</li><li>其他几个队列采用单层缓存设计，在较大发送量下，业务耗时增长更明显；2 MiB 时 rigtorp 与 TwoBufQueue 持平。</li></ul></section>'
    h+='<section class="report-note"><h2>注意</h2><p>这里面向的是允许批量交接、允许一批消息稍后处理的场景。生产者是热路径线程，消费者是辅助线程，目标是尽量减少队列对生产者后续业务的影响；在本次配置下，TwoBufQueue 更适合这个目标。本次测试的消费者持续运行，并没有等整批发送完才开始接收。</p><p>如果优先追求每条消息从发送到接收的时间极短，单层 SPSC 队列可避免额外的第二层复制，更有利于这个目标。</p></section>'
    h+=f'<section><details id="basic-parameters"><summary>基本参数</summary><div class="settings"><div>消息长度<strong>32 B</strong>完整内容随序号变化</div><div>每批发送量<strong>2–16 MiB</strong>2、4、8、16 MiB</div><div>业务数据表<strong>{p["selected_business_kib"]} KiB</strong>每次访问并更新 8 个节点</div><div>TwoBufQueue 第一层<strong>16 KiB</strong>4 × 4 KiB</div><div>第二层 / 原生槽位<strong>256 MiB</strong>元数据另计</div><div>每批起始积压<strong>0 B</strong>所有窗口均校验</div></div><p>回调统一执行 {p["callback_iterations"]} 次计算循环；每种发送量每轮 {p["final_window_repeats"]:,} 个窗口，采集 {p["samples_per_run"]:,} 组业务耗时，独立运行 {p["runs"]} 轮。分配、缓冲页预触碰、业务预热和排空不计入业务耗时。</p><h3>实际剩余消息</h3><p>所有窗口的起始积压均为 0，准备消息数均为 0。以下均值在发送和立即业务测量后采集，消费者期间持续处理：</p><ul>'
    for burst in bursts:
        descriptions=[]
        for queue in QUEUES:
            value=statistics.mean(r['pending_after_mean_bytes'] for r in by[queue,burst]['per_run'])/MIB
            descriptions.append(NAMES[queue]+f' {value:.2f} MiB')
        h+=f'<li>每批 {burst} MiB：'+html.escape('；'.join(descriptions))+'</li>'
    h+='</ul></details></section>'
    m=p['machine'];build=p['build']
    h+='<section id="test-environment"><details><summary>测试环境</summary><dl>'
    values=[('硬件',m.get('model','Apple Silicon')),('处理器',m.get('cpu','Apple M3')),('内存',str(m.get('memory_bytes',0)//(1024**3))+' GiB'),('操作系统',m.get('os','')),('编译环境',build.get('CMAKE_CXX_COMPILER_ID','')+' '+build.get('CMAKE_CXX_COMPILER_VERSION','')+' · C++20 · Release · '+build.get('CMAKE_CXX_FLAGS_RELEASE','')),('线程调度','生产者高 QoS，未硬绑核；复制和消费者线程不绑核'),('计时方式',result['timer_name']+f' · 刻度约 {result["timer_quantum_ns"]:.1f} ns'),('内存池深度统计',build.get('BRQ_ENABLE_POOL_DEPTH_STATS',''))]
    h+=''.join('<dt>'+html.escape(k)+'</dt><dd>'+html.escape(v)+'</dd>' for k,v in values)+'</dl></details></section>'
    h+='<section><h2>参数明细与复现</h2><details><summary>测量口径与复现命令</summary><p>起始状态：prefill=false、drain_before_burst=true。数据池、消息长度、业务工作集、回调循环、消费者调度参数与原积压实验固定参数一致；本次测量 2、4、8、16 MiB。原生 SPSC API 与上游源码保持原样。TwoBufQueue 多一个复制线程。</p>'
    h+=f'<pre>python3 benchmarks/burst/run_burst_empty.py --profile {profile_reference}</pre><p><a href="{prefix}/selected_config.json">完整初始化参数</a> · <a href="{prefix}/workflow.json">命令、环境与源码哈希</a> · <a href="{prefix}/formal/report_results.json">完整统计及每轮原值</a></p><details><summary>原始样本</summary><ul>'
    for queue in ('baseline',)+QUEUES:
        for run in range(1,p['runs']+1):h+=f'<li><a href="{prefix}/formal/raw/{p["selected_business_kib"]}k_{queue}_run{run}.json">{html.escape(queue)} · 第 {run} 轮</a></li>'
    h+='</ul></details></details><details><summary>全部参数</summary><pre>'+html.escape(json.dumps(p,ensure_ascii=False,indent=2))+'</pre></details></section></main></body></html>'
    report.parent.mkdir(parents=True,exist_ok=True);report.write_text(h)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',type=Path,default=ROOT/'docs/performance_data/burst_showcase_20261012/selected_config.json')
    parser.add_argument('--executable',type=Path)
    parser.add_argument('--output',type=Path,default=ROOT/'docs/performance_data'/('burst_from_empty_'+time.strftime('%Y%m%d_%H%M%S')))
    parser.add_argument('--report',type=Path,default=ROOT/'docs/reports/burst/performance_burst_from_empty.html')
    parser.add_argument('--build-dir',type=Path,default=ROOT/'build-burst-empty')
    parser.add_argument('--deps',type=Path,default=ROOT/'.deps/spsc')
    parser.add_argument('--compiler');parser.add_argument('--jobs',type=int,default=3)
    parser.add_argument('--resume',action='store_true');parser.add_argument('--render-only',action='store_true')
    args=parser.parse_args();folder=args.output.resolve();report=args.report.resolve()
    if args.render_only:render(folder,report,json.loads((folder/'formal/report_results.json').read_text()));return
    if sys.platform=='darwin' and 'AC Power' not in subprocess.check_output(['pmset','-g','batt'],text=True):raise RuntimeError('connect AC power')
    profile=json.loads(args.profile.read_text());profile['bursts_mib']=[2,4,8,16]
    profile['start_mode']='empty_each_burst';profile['benchmark_parameters']=dict(profile['benchmark_parameters'],prefill=False,drain_before_burst=True)
    args.recorded_build=profile['build'];args.compiler=choose_compiler(args.compiler,profile)
    executable=base.build(args);build=compiler_info(executable)
    if build.get('BRQ_ENABLE_POOL_DEPTH_STATS')!='OFF':raise ValueError('formal runs require pool-depth statistics OFF')
    if profile['runs']!=3 or profile['samples_per_run']<10000:raise ValueError('use recorded 3 runs and at least10000 samples')
    folder.mkdir(parents=True,exist_ok=True)
    sources={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for directory in ('src','include','benchmarks') for p in (ROOT/directory).rglob('*') if p.is_file() and p.suffix in ('.hpp','.cpp','.py','.css')}
    workflow={'schema':'burst_empty_workflow_v1','executable_sha256':tuning.sha(executable),'source_sha256':sources,'status':'running','input_profile_sha256':tuning.sha(args.profile),'consumer_pause':False}
    path=folder/'workflow.json'
    if path.exists():
        if not args.resume:raise ValueError('output already has a workflow')
        old=json.loads(path.read_text())
        for key in ('executable_sha256','source_sha256','input_profile_sha256'):
            if old[key]!=workflow[key]:raise ValueError('resume inputs changed')
    base.save(path,workflow)
    profile['build']=build;profile['machine']=base.machine_info()
    base.save(folder/'selected_config.json',profile);base.save(folder/'environment.json',{'machine':profile['machine']})
    manifest={'records':[],'runs':3,'bursts_mib':profile['bursts_mib']}
    print('Empty-start formal run: 3 runs, 2500 windows/size, 10000 business groups/size/run, callback loops='+str(profile['callback_iterations']),flush=True)
    for run in range(1,profile['runs']+1):
        for case in tuning.CASES:
            data=base.run_case(executable,folder/'formal',profile['selected_business_kib'],case,run,profile['bursts_mib'],profile['final_window_repeats'],profile['callback_iterations'],profile['process_timeout'],args.resume,progress=128,queue_parameters=profile['queue_parameters'][case[0]],benchmark_parameters=profile['benchmark_parameters'])
            validate_empty(data,profile)
            manifest['records'].append({'queue':case[0],'run':run,'file':f'raw/{profile["selected_business_kib"]}k_{case[0]}_run{run}.json'})
            base.save(folder/'formal/manifest.json',manifest)
    result=summarize(folder,profile);render(folder,report,result)
    workflow['status']='complete';workflow['report']=str(report);base.save(path,workflow)
    print('Saved:',report,flush=True)


if __name__=='__main__':main()
