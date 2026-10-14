#!/usr/bin/env python3
"""Run identical live-callback bursts; save every command and unfiltered result.

Build test_burst_cache via BRQ_BUILD_SPSC_BENCHMARKS first. Each process runs the
full ascending sweep once. Three processes per configuration are averaged.
Callback calibration and both external copy variants remain in the data folder.
"""
import argparse
import hashlib
import html
import json
import os
import pathlib
import platform
import statistics
import subprocess
import time

MIB = 1024 * 1024
POOLS = [2**i * 1024 for i in range(1, 8)]
BURSTS = [2**i * 1024 for i in range(4, 18)]


def configurations():
    yield 'baseline', 'baseline', 32768, {}
    for pool in POOLS:
        yield f'twobuf_{pool//1024}k', 'two_buf_queue', pool, {}
    for flag in (False, True):
        yield f'rwq_zero_{int(flag)}', 'readerwriterqueue', 32768, {
            'spsc_capacity': 8371713, 'spsc_idle_spins': 0,
            'spsc_idle_sleep_ns': 12500, 'readerwriterqueue_copy_after_zero': flag}
        yield f'rigtorp_prefetch_{int(flag)}', 'rigtorp', 32768, {
            'spsc_capacity': 8388603, 'spsc_idle_spins': 4000,
            'spsc_idle_sleep_ns': 1000, 'rigtorp_write_prefetch': flag}
    yield 'atomic', 'atomic_queue', 32768, {
        'spsc_capacity': 8388608, 'spsc_idle_spins': 2000,
        'spsc_idle_sleep_ns': 10000}


def command(executable, transport, pool, flags, loops, **extra):
    values = {'transport': transport, 'source_pool_bytes': pool,
              'callback_iterations': loops, 'repeats': 1, **flags, **extra}
    args = [str(executable)]
    for key, value in values.items():
        args.extend(['--'+key, str(value).lower() if isinstance(value, bool) else str(value)])
    return args


def execute(cmd, path):
    start = time.monotonic()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=200)
    if proc.returncode:
        path.with_suffix('.stderr.txt').write_text(proc.stderr)
        raise RuntimeError(f'{path.name}: exit {proc.returncode}: {proc.stderr}')
    data = json.loads(proc.stdout)
    assert data['verified'] and data['accepted'] == data['completed']
    data['invocation'] = {'argv': cmd, 'wall_seconds': time.monotonic()-start,
                          'stderr': proc.stderr, 'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
    path.write_text(json.dumps(data, indent=2)+'\n')
    return data


def calibrate(executable, folder):
    # One common callback is chosen from a real live-consumer, 128MiB burst.
    # Preparation does not stop the consumer and is not timed as producer work.
    calibration = folder/'calibration'
    calibration.mkdir(exist_ok=True)
    definitions = [x for x in configurations() if x[1] in
                   ('readerwriterqueue', 'rigtorp', 'atomic_queue')]
    trials = []
    for loops in (128, 256, 512):
        observations = []
        for label, transport, pool, flags in definitions:
            cmd = command(executable, transport, pool, flags, loops, burst_bytes=128*MIB)
            data = execute(cmd, calibration/f'{label}_loops{loops}.json')
            p = data['points'][0]
            occupancy = p['pending_after']/(256*MIB)
            observations.append(occupancy)
            trials.append({'label': label, 'loops': loops, 'occupancy': occupancy,
                           'wall_seconds': data['invocation']['wall_seconds']})
            print(f'calibration {label} loops={loops}: pending={occupancy:.1%}, '
                  f'wall={data["invocation"]["wall_seconds"]:.2f}s', flush=True)
        if min(observations) >= .75:
            (folder/'calibration.json').write_text(json.dumps({'chosen_loops': loops, 'trials': trials}, indent=2))
            return loops
    raise RuntimeError('Calibration failed to retain a 75% pilot margin; inspect raw results before extending callback work')


def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args):
    folder = args.output.resolve()
    folder.mkdir(parents=True, exist_ok=True)
    raw = folder/'raw'; raw.mkdir(exist_ok=True)
    loops = args.callback_iterations or calibrate(args.executable.resolve(), folder)
    configs = list(configurations())
    plan = {'schema': 'burst_cache_plan_v1', 'callback_iterations': loops,
            'independent_runs': args.runs, 'writer_pool_totals': POOLS, 'bursts': BURSTS,
            'queue_data_bytes': 256*MIB, 'consumer_pause': False, 'prefill': args.prefill,
            'machine': platform.platform(), 'processor': platform.machine(),
            'executable_sha256': fingerprint(args.executable),
            'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
            'records': []}
    for name in ('burst_cache.hpp','test_burst_cache.cpp','burst_prefault.cpp','../hot_path/hot_path_cache.hpp',
                 '../tunable/adapters.hpp','../tunable/performance_test.hpp','../tunable/config.hpp'):
        path = pathlib.Path(__file__).parent/name
        if path.exists(): plan.setdefault('source_sha256', {})[name] = fingerprint(path)
    # Rotate process order, with no overlapping tests or CPU-heavy subprocesses.
    for repetition in range(1, args.runs+1):
        order = configs[(repetition-1)*4:] + configs[:(repetition-1)*4]
        for label, transport, pool, flags in order:
            filename = f'{label}_run{repetition}.json'
            cmd = command(args.executable.resolve(), transport, pool, flags, loops, prefill=args.prefill)
            data = execute(cmd, raw/filename)
            assert [p['burst_bytes'] for p in data['points']] == BURSTS
            assert not data['hard_affinity']
            if platform.system() == 'Darwin': assert data['qos_applied']
            assert data['formal'] == sum(BURSTS)//32
            plan['records'].append({'label': label, 'repetition': repetition, 'file': 'raw/'+filename})
            (folder/'manifest.json').write_text(json.dumps(plan, indent=2)+'\n')
            after = [p['pending_after']/(256*MIB) for p in data['points']]
            reached = sum(p['setup_target_reached'] for p in data['points'])
            print(f'{label} run{repetition}: {data["invocation"]["wall_seconds"]:.2f}s, '
                  f'pending {min(after):.0%}..{max(after):.0%}, targets {reached}/{len(BURSTS)}, '
                  f'RSS={data["rss_mib"]:.0f}MiB', flush=True)
    plan['finished_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    (folder/'manifest.json').write_text(json.dumps(plan, indent=2)+'\n')
    return folder


def aggregate(folder):
    manifest = json.loads((folder/'manifest.json').read_text())
    runs = {}
    for record in manifest['records']:
        data = json.loads((folder/record['file']).read_text())
        assert data['verified'] and data['formal'] == sum(BURSTS)//32
        runs.setdefault(record['label'], []).append(data)
    checksums = {d['business_checksum'] for seq in runs.values() for d in seq}
    assert len(checksums) == 1, 'All transports must execute identical business queries'
    rows = []
    for label, seq in runs.items():
        assert len(seq) == manifest['independent_runs']
        for index, burst in enumerate(BURSTS):
            points = [d['points'][index] for d in seq]
            avg = lambda key: statistics.mean(p[key] for p in points)
            after = [statistics.mean(p['after_business_ns'][i] for p in points) for i in range(4)]
            rows.append({'label': label, 'transport': seq[0]['transport'],
                         'source_pool_bytes': seq[0]['source_pool_bytes'], 'burst_bytes': burst,
                         'before_ns': avg('pre_business_ns'), 'first16_ns': after[0],
                         'after64_ns': statistics.mean(after), 'after_groups_ns': after,
                         'writer_m_messages_s': avg('writer_m_messages_s'),
                         'pending_before_percent': avg('pending_before')/(256*MIB)*100,
                         'pending_after_percent': avg('pending_after')/(256*MIB)*100,
                         'output_pending_after_percent': avg('output_pending_after')/(256*MIB)*100,
                         'backpressure_percent': avg('backpressured_messages')/(burst/32)*100,
                         'backpressure_count': sum(p['backpressured_messages'] for p in points),
                         'retries': sum(p['retries'] for p in points),
                         'target_reached_runs': sum(p['setup_target_reached'] for p in points),
                         'writer_ns_samples': [p['writer_ns'] for p in points],
                         'first16_ns_samples': [p['after_business_ns'][0] for p in points],
                         'after64_ns_samples': [statistics.mean(p['after_business_ns']) for p in points],
                         'rss_mib': statistics.mean(d['rss_mib'] for d in seq),
                         'wall_seconds': statistics.mean(d['invocation']['wall_seconds'] for d in seq)})
    summary = {'manifest': manifest, 'rows': rows}
    (folder/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    return summary, runs


# Report generation is below; --render-only never runs a benchmark.
def render(folder, report):
    summary, runs = aggregate(folder)
    rows = summary['rows']; manifest = summary['manifest']
    by = {(r['label'], r['burst_bytes']): r for r in rows}
    labels = [x[0] for x in configurations()]
    business_kib = runs[labels[0]][0]['business_bytes']/1024
    esc = html.escape
    def size(n):
        return f'{n/MIB:g} MiB' if n >= MIB else f'{n/1024:g} KiB'
    def title(label):
        if label == 'baseline': return '业务基线（无队列）'
        if label.startswith('twobuf_'): return f'TwoBufQueue · 第一层 {size(by[label, BURSTS[0]]["source_pool_bytes"])}'
        if label.startswith('rwq_'): return 'ReaderWriterQueue · '+('先清零再复制' if label.endswith('1') else '成员复制')
        if label.startswith('rigtorp_'): return 'rigtorp::SPSCQueue · '+('写预取' if label.endswith('1') else '无写预取')
        return 'atomic_queue'
    def table(headers, body):
        return '<div class="scroll"><table><thead><tr>'+''.join('<th>'+h+'</th>' for h in headers)+'</tr></thead><tbody>'+body+'</tbody></table></div>'
    def perf(burst):
        body=''; base=by['baseline',burst]['after64_ns']
        for label in labels:
            row=by[label,burst]; tb=label.startswith('twobuf_'); baseline=label=='baseline'
            count=burst//32*manifest['independent_runs']
            rate='—' if baseline else f'{row["writer_m_messages_s"]:.1f}'
            body+=f'<tr class="{"tb" if tb else "base" if baseline else ""}"><th>{title(label)}</th>'
            body+=f'<td>{rate}</td><td>{row["before_ns"]:.1f}</td><td>{row["after64_ns"]:.1f}</td>'
            body+=f'<td>{row["after64_ns"]/base:.2f}×</td>'
            body+=f'<td>{row["pending_after_percent"]:.0f}%</td>'
            body+=f'<td>{row["backpressure_percent"]:.0f}%<small>{row["backpressure_count"]:,} / {count:,}</small></td></tr>'
        return table(['实现 / 第一层总池','突发写入<br>M条/s ↑','突发前业务<br>ns/次 ↓','突发后前64次业务<br>ns/次 ↓','相对无队列<br>耗时倍数 ↓','实际未消费<br>数据占用率','背压%'],body)
    def recovery(burst):
        body=''
        for label in labels:
            row=by[label,burst]
            body+=f'<tr class="{"tb" if label.startswith("twobuf_") else "base" if label=="baseline" else ""}"><th>{title(label)}</th>'
            body+=''.join(f'<td>{v:.1f}</td>' for v in row['after_groups_ns'])+'</tr>'
        return table(['实现','第1–16次 ns/次','第17–32次 ns/次','第33–48次 ns/次','第49–64次 ns/次'],body)
    default=4*MIB
    totalwall=sum(d['invocation']['wall_seconds'] for seq in runs.values() for d in seq)
    actual=[p['pending_after']/(256*MIB)*100 for seq in runs.values() for d in seq if d['transport'] not in ('baseline','two_buf_queue') for p in d['points']]
    source=os.path.relpath(folder,report.parent).replace(os.sep,'/')
    h='''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>TwoBufQueue · 小第一层与持续慢回调的突发测试</title><style>
:root{--bg:#f3f6fb;--ink:#182b44;--sub:#556980;--tb:#dff4ee;--brand:#087f6e}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.65 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}main{max-width:1260px;margin:auto;padding:32px 24px}h1{font-size:36px;line-height:1.25}h2{font-size:23px;margin:0 0 15px}h3{font-size:18px}p{margin:10px 0}.eyebrow{color:#64ddc3;font-weight:750;letter-spacing:2px}.hero{background:#102d40;color:#fff;border-radius:22px;padding:32px}.hero p{color:#c3d9e5}.cards{display:grid;grid-template-columns:repeat(3,1fr);gap:14px;margin:24px 0}.card{padding:20px;background:#fff;border:1px solid #dce5ee;border-radius:16px}.number{font-size:30px;font-weight:800;color:var(--brand)}.muted,small{color:var(--sub)}section{padding:24px;background:#fff;border-radius:16px;border:1px solid #dce5ee;margin:22px 0}.scroll{overflow:auto}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums;font-size:14px}th,td{padding:11px 12px;border-bottom:1px solid #dce5ee;text-align:right;white-space:nowrap}th:first-child{text-align:left}thead th{background:#edf2f8;color:#435a72}tr.tb>*{background:var(--tb);font-weight:750;color:#075c4e}tr.base>*{background:#f5f7fa;color:#697d91}small{display:block;font-size:11px;font-weight:400}.note{padding:14px 18px;background:#f0f5fa;border-radius:10px;border-left:4px solid #7394b1}.pipeline{display:grid;grid-template-columns:1fr 1fr;gap:18px}.pipeline>div{background:#f2f7fa;padding:20px;border-radius:12px}.chip{display:inline-block;padding:6px 10px;border-radius:9px;background:#ccefe6;font-weight:700}.queue{background:#dae7f5}summary{cursor:pointer;font-weight:700;padding:8px 0}pre{font-size:12px;max-height:540px;overflow:auto;background:#f6f8fb;padding:18px;border-radius:10px;line-height:1.45}a{color:var(--brand)}.badge{font-size:12px;border:1px solid #486272;border-radius:20px;padding:4px 10px;margin-right:6px;display:inline-block}select{font:inherit;padding:7px 15px;border:1px solid #a9bfd0;border-radius:8px;background:#fff;color:var(--ink)}.control{display:flex;gap:15px;align-items:center;margin:12px 0 22px}@media(max-width:700px){main{padding:16px}.hero{padding:22px}h1{font-size:28px}.cards,.pipeline{grid-template-columns:1fr}section{padding:16px}}
</style></head><body><main>'''
    h+='''<div class="hero"><div class="eyebrow">TWOBUFQUEUE / BURST &amp; BUSINESS CACHE</div><h1>后台持续积压时，<br>写线程仍可复用小数据池</h1><p>先让业务数据变热，连续写入一批消息，再立即执行业务计算。慢回调全程运行，直接观察突发后的业务耗时与恢复过程。</p><span class="badge">Apple M3</span><span class="badge">3次独立进程取平均</span><span class="badge">高 QoS · 未硬绑核</span><span class="badge">全部32 B消息校验</span></div>'''
    h+=f'''<div class="cards"><div class="card"><div class="muted">TwoBufQueue 第一层总数据池</div><div class="number">2–128 KiB</div><p>最小池为 4 × 512 B；第二层固定 256 MiB。</p></div><div class="card"><div class="muted">每次突发的数据量</div><div class="number">16 KiB–128 MiB</div><p>14档；所有实现使用相同正式消息。</p></div><div class="card"><div class="muted">开源对照 · 实际未消费数据</div><div class="number">{min(actual):.0f}%–{max(actual):.0f}%</div><p>统一每条回调执行 {manifest["callback_iterations"]} 次计算循环。</p></div></div>'''
    h+='''<section><h2>本次展示的结构优势</h2><div class="pipeline"><div><b>TwoBufQueue</b><p>业务线程 → <span class="chip">2–128 KiB 第一层</span><br>复制线程 → <span class="chip queue">256 MiB 第二层</span> → 慢回调线程</p></div><div><b>原生 SPSC 对照</b><p>业务线程 → <span class="chip queue">约256 MiB 数据区</span><br>消费线程 → 相同慢回调</p></div></div><p>先按本次突发量预留空间，把未消费数据调整到“80%容量 − 本次突发字节数”附近；随后热身业务表、执行突发、立即测量业务。回调不暂停，不加人为睡眠；准备与最终排空不计入突发写入和业务耗时。</p><p class="note">容量指数据缓冲大小，另有对象、索引和分配器开销。未消费字节数 =（成功写入条数 − 已执行回调条数）×32，<b>不是直接测出的 L1 占用或缓存命中率</b>。TwoBufQueue 使用额外复制线程；实际业务耗时包含后台竞争及线程调度的影响。</p></section>'''
    h+='''<section><h2>完整性能结果</h2><p>绿色整行是 TwoBufQueue。各突发量都保留全部第一层参数和开源复制选项，没有按单项指标隐藏较差结果。</p><div class="control"><label for="burst">选择突发量</label><select id="burst">'''
    for burst in BURSTS:
        h+=f'<option value="{burst}"{" selected" if burst==default else ""}>{size(burst)}</option>'
    h+='</select></div>'
    for burst in BURSTS:
        h+=f'<div class="burst-panel" data-burst="{burst}"{" hidden" if burst!=default else ""}><h3>{size(burst)} 突发 · {burst//32:,}条32 B消息</h3>'+perf(burst)
        h+='<details><summary>业务耗时的四段恢复过程</summary>'+recovery(burst)+'</details></div>'
    h+='''<p class="note"><b>指标口径：</b>突发吞吐包含生成完整32 B消息、普通write/原生入队和失败重试。业务指标只计业务计算：每组16次，每次访问更新8个依赖节点；“前64次”是四组均值。三次进程的对应结果取算术平均。它们<b>不是单条write延时或p50/p99</b>。背压%=首次写入失败的正式消息数/正式消息数，显示整数，精确条数保留在下一行。TwoBufQueue 的尾块在这64次业务计算后交接，计时内采用完整块发布。</p></section>'''
    h+='<section><details><summary>展开全部14档突发结果（可打印）</summary>'
    for burst in BURSTS:
        h+=f'<h3>{size(burst)}</h3>'+perf(burst)
    h+='</details></section>'
    init=''
    for label in labels:
        data=runs[label][0]; q=data['queue_config']; is_tb=label.startswith('twobuf_')
        pool=size(data['source_pool_bytes']) if is_tb else '—'
        block=size(q['source_block_size']) if is_tb else '—'
        output=f'{q["output_pool_max"]:,} × {size(q["output_block_size"])}' if is_tb else f'{data["actual_native_capacity"]:,}槽'
        init+=f'<tr class="{"tb" if is_tb else ""}"><th>{title(label)}</th><td>{pool}</td><td>{block}</td><td>{output}</td><td>{statistics.mean(d["rss_mib"] for d in runs[label]):.0f}</td><td>{statistics.mean(d["invocation"]["wall_seconds"] for d in runs[label]):.2f}</td></tr>'
    h+='<section><h2>初始化参数与测试时长</h2>'+table(['实现 / 选项','第一层总池','Writer/Reader单块','第二层或原生容量','进程峰值RSS MiB','完整遍历秒/次'],init)
    h+=f'<p>固定业务表{business_kib:g} KiB，随机种子42，每次8次依赖读取与更新；每次突发前512次热身，正式测量前16次业务与后64次业务。消息长度固定32 B，所有4个8 B字段均随序号变化。各配置14档突发合计 {sum(BURSTS)//32:,} 条正式消息，准备消息另计。{len(manifest["records"])}个正式进程总耗时约 {totalwall/60:.1f} 分钟，另有校准和正确性检查。</p>'
    h+='''<p>源池4块，完整块发布（publish_count=UINT32_MAX），不扫描current，保留普通write现有的首字节volatile读回；第二层单块与第一层单块相同，总数据容量恒为256 MiB。三个原生队列的槽位存储均为256 MiB，元数据另计。ReaderWriterQueue为16,384个512槽块，每块留一个空槽，有效容量255.5 MiB；rigtorp的padding与备用槽已计入256 MiB，atomic_queue另有状态数组。极小Reader块会增加对象与索引数量，RSS已经如实列出。</p><p>计时前预触碰缓冲页；本测试通过独立链接单元在aligned allocation返回前清零原始存储，原生队列另用真实入队/出队预触碰。该准备不修改任何活对象、组件源码或正式写入逻辑。普通消息由寄存器/小栈对象生成，避免大型输入数组污染业务缓存。</p><details><summary>每种配置的全部初始化字段与命令</summary>'''
    for label in labels:
        data=runs[label][0]
        config={k:v for k,v in data.items() if k in ('queue_config','source_pool_bytes','business_bytes','business_accesses','query_group','query_groups','warm_queries','prefill','setup_timeout_ms','target_percent','progress_chunk','repeats','actual_native_capacity','timer_name','timer_quantum_ns','qos_applied','hard_affinity')}
        config['queue_config']=dict(config['queue_config'])
        config['effective_formal_messages']=data['formal']
        config['ignored_generic_harness_fields']=['messages','warmup_messages','sample_every','max_samples','latency_batch_size','call_batch_size','fixed_round_boundaries','round_storage_bytes']
        config['note']='本独立runner使用突发列表；通用字段完整保留，不代表实际执行了原分轮测试。占用目标为第二层80%，连续消费并在点间调整，单次突发最多128MiB。'
        config['argv']=data['invocation']['argv']
        h+=f'<details><summary>{title(label)}</summary><pre>'+esc(json.dumps(config,ensure_ascii=False,indent=2))+'</pre></details>'
    h+='</details></section>'
    h+='<section><h2>原始数据与核查</h2><p>正式结果全部通过消息条数、FIFO顺序及每条32 B内容校验；所有实现的业务校验和一致。源代码与可执行文件哈希、每条命令、三次独立结果均已保存。</p>'
    h+=f'<p><a href="{source}/summary.json">全部汇总JSON</a> · <a href="{source}/manifest.json">运行清单与哈希</a> · <a href="{source}/diagnostics/full128/manifest.json">128次回调循环的初轮数据</a></p>'
    h+='<details><summary>三次业务耗时与吞吐原值</summary>'
    rawbody=''
    for burst in BURSTS:
        for label in labels:
            row=by[label,burst]
            rawbody+=f'<tr class="{"tb" if label.startswith("twobuf_") else ""}"><th>{size(burst)} · {title(label)}</th><td>'+ ' / '.join(f'{x:.1f}' for x in row['after64_ns_samples'])+'</td><td>'+('—' if label=='baseline' else ' / '.join(f'{(burst/32)/x*1000:.1f}' for x in row['writer_ns_samples']))+'</td></tr>'
    h+=table(['突发量 / 实现','前64次业务 ns/次 · 3次原值','突发吞吐 M条/s · 3次原值'],rawbody)+'</details>'
    h+='<details><summary>正式原始文件</summary><ul>'
    for record in manifest['records']:
        h+=f'<li><a href="{source}/{record["file"]}">{esc(record["label"])} · 第{record["repetition"]}次</a></li>'
    h+='</ul></details><p class="muted">本报告展示这台M3机器和本负载下的结果；没有用耗时反推缓存命中率，也没有修改开源队列内部实现。</p></section>'
    h+='''</main><script>document.getElementById('burst').addEventListener('change',function(){for(const p of document.querySelectorAll('.burst-panel'))p.hidden=p.dataset.burst!==this.value;});</script></body></html>'''
    report.write_text(h)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable', type=pathlib.Path, required=True)
    p.add_argument('--output', type=pathlib.Path, required=True)
    p.add_argument('--report', type=pathlib.Path)
    p.add_argument('--runs', type=int, default=3)
    p.add_argument('--callback-iterations', type=int, default=0, help='0 calibrates one common value')
    p.add_argument('--prefill', type=lambda x: {'true': True, 'false': False}[x.lower()], default=True)
    p.add_argument('--render-only', action='store_true')
    args = p.parse_args()
    if args.runs < 1 or args.callback_iterations < 0: p.error('runs must be positive and callback iterations nonnegative')
    if not args.render_only: run(args)
    if args.report: render(args.output.resolve(), args.report.resolve())


if __name__ == '__main__': main()
