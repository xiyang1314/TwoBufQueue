#!/usr/bin/env python3
"""Sequential, resumable hot-path memory showcase. No concurrent benchmarks."""
import argparse
import csv
import hashlib
import html
import json
import platform
import os
import random
import statistics
import subprocess
import time
from pathlib import Path

MiB = 1024 * 1024
LABELS = {'baseline': '业务基线（无队列）', 'two_buf_queue': 'TwoBufQueue',
          'readerwriterqueue': 'ReaderWriterQueue', 'rigtorp': 'rigtorp::SPSCQueue', 'atomic_queue': 'atomic_queue'}
SCENARIOS = {'paused': '可控后台积压', 'slow': '真实慢回调', 'fast': '即时消费'}
WEIGHTS = {'m_business_per_second': .35, 'p50': .30, 'p99': .20, 'p999': .15}

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def common():
    return {'messages': 10_000_000, 'business_bytes': 96*1024, 'business_accesses': 8,
            'business_warmup_steps': 20_000, 'preparation_rounds': 2, 'source_block_size': 65536,
            'source_pool_min': 4, 'source_pool_max': 4, 'source_queue_capacity': 16,
            'output_block_size': 65536, 'output_pool_min': 4096, 'output_pool_max': 4096,
            'output_frame_capacity': 4, 'output_queue_capacity': 4098,
            'is_scan_current': False, 'publish_count': 4294967295,
            'round_storage_bytes': (256*MiB//5)*4, 'spsc_buffer_bytes': 256*MiB,
            'fixed_round_boundaries': True, 'prefault_producer_buffers': True,
            'producer_qos': 'user_interactive' if platform.system() == 'Darwin' else 'unchanged',
            'latency_batch_size': 1000, 'timer_calibration_samples': 16384, 'timeout_seconds': 180}

def native(t, alternative=False):
    if t == 'readerwriterqueue':
        return {'spsc_capacity': 8371713, 'readerwriterqueue_copy_after_zero': not alternative,
                'spsc_idle_spins': 0, 'spsc_idle_sleep_ns': 12500, 'retry_yield_every': 1024}
    if t == 'rigtorp':
        return {'spsc_capacity': 8388603, 'rigtorp_write_prefetch': not alternative,
                'spsc_idle_spins': 4000, 'spsc_idle_sleep_ns': 1000, 'retry_yield_every': 1024}
    if t == 'atomic_queue':
        return {'spsc_capacity': 8388608, 'spsc_idle_spins': 2000, 'spsc_idle_sleep_ns': 10000,
                'retry_yield_every': 1024}
    return {}

def blocks(kib):
    b = kib*1024
    return {'source_block_size': b, 'output_block_size': b, 'output_pool_min': 256*MiB//b,
            'output_pool_max': 256*MiB//b, 'output_queue_capacity': 256*MiB//b+2}

def run_case(exe, directory, case, repetition, measurement, validate=False):
    key = f"{case['id']}__{'validation' if validate else measurement}__{repetition}"
    dest = directory/'raw'/f'{key}.json'
    args = {**common(), **case['parameters'], 'measurement': measurement}
    if validate:
        args.update(messages=100_000, full_oracle=True, preparation_rounds=1)
    command = [str(exe)]
    for k,v in args.items():
        command += ['--'+k, str(v).lower() if isinstance(v,bool) else str(v)]
    signature = hashlib.sha256(json.dumps([sha(exe), command]).encode()).hexdigest()
    if dest.exists():
        saved = json.loads(dest.read_text())
        if saved['signature'] != signature:
            raise RuntimeError(f'cannot reuse stale result {dest}')
        return saved['result']
    t = time.time()
    done = subprocess.run(command,capture_output=True,text=True,timeout=210)
    if done.returncode:
        (directory/'raw'/f'{key}.error.txt').write_text(done.stdout+done.stderr)
        raise RuntimeError(f'{key}: {done.stderr}')
    result = json.loads(done.stdout)
    if not result['verified'] or result['consumed'] != args['messages']:
        raise RuntimeError(f'{key}: verification failed')
    if validate and args['transport'] != 'baseline' and result['oracle_checked'] != args['messages']:
        raise RuntimeError(f'{key}: oracle incomplete')
    dest.write_text(json.dumps({'case': case, 'repetition': repetition, 'command': command,
                               'signature': signature, 'wall_seconds': time.time()-t,
                               'result': result},indent=2)+'\n')
    print(key, f"{result['m_business_per_second']:.3f} M iterations/s", flush=True)
    return result

def measure(exe, directory, cases):
    for case in cases:
        run_case(exe,directory,case,0,'throughput',validate=True)
    jobs = [(c,rep,m) for rep in range(1,4) for c in cases for m in ['throughput','groups']]
    random.Random(20261012).shuffle(jobs)
    for c,rep,m in jobs:
        run_case(exe,directory,c,rep,m)

def aggregate(directory, case):
    rows = [json.loads(p.read_text())['result'] for p in sorted((directory/'raw').glob(case['id']+'__*.json'))
            if '__validation__' not in p.name]
    th = [r for r in rows if r['measurement']=='throughput']
    la = [r for r in rows if r['measurement']=='groups']
    if len(th)!=3 or len(la)!=3: raise RuntimeError('missing three-repeat measurements')
    # All transports execute exactly the same business operations and sequence.
    if len({r['business_checksum'] for r in rows}) != 1: raise RuntimeError('business checksum drift')
    mean = lambda key: statistics.mean(r[key] for r in th)
    return {'case': case, 'queue_config': th[0]['queue_config'], 'throughput':mean('m_business_per_second'),
            'throughput_repeats':[r['m_business_per_second'] for r in th],
            'throughput_cv_percent':statistics.stdev(r['m_business_per_second'] for r in th)/mean('m_business_per_second')*100,
            **{q:statistics.mean(r['group_average_iteration_ns'][q] for r in la) for q in ['p50','p99','p999']},
            'backpressured_messages':sum(r['backpressured_messages'] for r in th),
            'messages':sum(r['queue_config']['messages'] for r in th),
            'callback_to_producer_ratio':statistics.mean(r['callback_seconds']/r['producer_seconds'] for r in th),
            'business_checksum':th[0]['business_checksum'],
            'group_samples':sum(r['group_samples'] for r in la),
            'copied_while_paused_bytes':sum(r['copied_while_paused_bytes'] for r in th),
            'pause_confirmed_rounds':sum(r['pause_confirmed_rounds'] for r in th),
            'completed_rounds':sum(r['completed_rounds'] for r in th),
            'minor_faults_timed_process':sum(r['minor_faults_timed_process'] for r in th),
            'peak_rss_mib':statistics.mean(r['peak_rss_mib'] for r in th),
            'source_bytes':th[0]['queue_config']['source_block_size']*th[0]['queue_config']['source_pool_max'],
            'actual_native_capacity':th[0]['actual_capacity']}

def score_pick(rows):
    best = {'throughput':max(r['throughput'] for r in rows), **{q:min(r[q] for r in rows) for q in ['p50','p99','p999']}}
    for r in rows:
        r['selection_score'] = .35*r['throughput']/best['throughput'] + sum(WEIGHTS[q]*best[q]/r[q] for q in ['p50','p99','p999'])
    # Require an adjacent candidate within 15% throughput: reject isolated peaks.
    ordered = sorted(rows,key=lambda r:r['source_bytes'])
    supported=[]
    for i,r in enumerate(ordered):
        neighbors=ordered[max(0,i-1):i]+ordered[i+1:i+2]
        if any(n['throughput'] >= r['throughput']*.85 and n['p50'] <= r['p50']*1.2 for n in neighbors): supported.append(r)
    pool = supported or rows
    peak = max(r['selection_score'] for r in pool)
    # Showcase memory separation: use the smallest supported pool within 2%
    # of the best four-metric score, instead of spending memory for a tiny gain.
    return min((r for r in pool if r['selection_score'] >= peak*.98),key=lambda r:r['source_bytes'])

def html_report(directory, summary, report_path=None):
    report_path = (report_path or directory.parent.parent/'performance_hot_path_cache.html').resolve()
    report_path.parent.mkdir(parents=True,exist_ok=True)
    esc = html.escape
    data_href = esc(Path(os.path.relpath(directory,report_path.parent)).as_posix())
    fmt = lambda n: f'{n:,.2f}'
    def row(r,baseline=False):
        t=r['case']['parameters']['transport']; style='tb' if t=='two_buf_queue' else 'base' if baseline else ''
        bp=r['backpressured_messages']/r['messages']*100
        return f"<tr class='{style}'><th>{LABELS[t]}</th><td>{fmt(r['throughput'])}</td><td>{fmt(r['p50'])}</td><td>{fmt(r['p99'])}</td><td>{fmt(r['p999'])}</td><td>{bp:.0f}%<small>{r['backpressured_messages']:,} / {r['messages']:,}</small></td></tr>"
    def table(rows):
        return "<div class='scroll'><table><thead><tr><th>实现</th><th>业务吞吐<br>M次/s ↑</th><th>p50<br>ns/次 ↓</th><th>p99<br>ns/次 ↓</th><th>p999<br>ns/次 ↓</th><th>背压%</th></tr></thead><tbody>"+''.join(row(r,r['case']['parameters']['transport']=='baseline') for r in rows)+"</tbody></table></div>"
    main=summary['main']['paused'];tb=next(r for r in main if r['case']['parameters']['transport']=='two_buf_queue')
    peers=[r for r in main if r['case']['parameters']['transport'] not in ['baseline','two_buf_queue']]
    fastest=max(peers,key=lambda r:r['throughput']);gain=(tb['throughput']/fastest['throughput']-1)*100
    copied_percent=tb['copied_while_paused_bytes']/(tb['messages']*32)*100
    shrink=256*MiB/tb['source_bytes']; hot=f"{tb['source_bytes']//1024:,} KiB"
    h=f"业务吞吐 +{gain:.1f}%" if gain>0 else f"最快开源对照的 {tb['throughput']/fastest['throughput']*100:.1f}%"
    body=f"""<!doctype html><html lang='zh-CN'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>TwoBufQueue · 热路径小缓存与后台大积压</title>
<style>
:root{{--bg:#f3f6fb;--ink:#182b44;--sub:#556980;--tb:#dff4ee;--brand:#087f6e}}*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--ink);font:16px/1.65 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}}main{{max-width:1160px;margin:auto;padding:32px 24px}}h1{{font-size:36px;line-height:1.25;max-width:900px}}h2{{font-size:23px;margin:0 0 15px}}p{{margin:10px 0}}.eyebrow{{color:var(--brand);font-weight:750;letter-spacing:2px}}.hero{{background:#102d40;color:#fff;border-radius:22px;padding:32px;box-shadow:0 10px 40px #102d4020}}.hero p{{color:#c3d9e5}}.cards{{display:grid;grid-template-columns:repeat(3,1fr);gap:14px;margin:24px 0}}.card{{padding:20px;background:white;border:1px solid #dce5ee;border-radius:16px}}.number{{font-size:32px;font-weight:800;color:var(--brand)}}.muted,small{{color:var(--sub)}}section{{padding:24px;background:white;border-radius:16px;border:1px solid #dce5ee;margin:22px 0}}.scroll{{overflow:auto}}table{{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums;font-size:15px}}th,td{{padding:13px 15px;border-bottom:1px solid #dce5ee;text-align:right;white-space:nowrap}}th:first-child{{text-align:left}}thead th{{background:#edf2f8;color:#435a72}}tr.tb>*,.tb{{background:var(--tb);font-weight:750;color:#075c4e}}tr.base>*{{background:#f5f7fa;color:#697d91}}small{{display:block;font-size:12px;font-weight:400}}.note{{padding:14px 18px;background:#f0f5fa;border-radius:10px;border-left:4px solid #7394b1}}.pipeline{{display:grid;grid-template-columns:1fr 1fr;gap:18px}}.pipeline>div{{background:#f2f7fa;padding:20px;border-radius:12px}}.chip{{display:inline-block;padding:8px 12px;border-radius:9px;background:#ccefe6;margin:5px 0;font-weight:700}}.queue{{background:#dae7f5}}summary{{cursor:pointer;font-weight:700;padding:8px 0}}pre{{font-size:12px;max-height:540px;overflow:auto;background:#f6f8fb;padding:18px;border-radius:10px;line-height:1.45}}a{{color:var(--brand)}}.bar{{height:12px;border-radius:6px;background:#c1d6ee;margin:7px 0}}.bar.tb{{background:#159b84}}.badge{{font-size:12px;border:1px solid #486272;border-radius:20px;padding:4px 10px;margin-right:6px;display:inline-block}}@media(max-width:700px){{main{{padding:16px}}.hero{{padding:22px}}h1{{font-size:28px}}.cards,.pipeline{{grid-template-columns:1fr}}section{{padding:16px}}}}
</style><main><div class='hero'><div class='eyebrow'>TWOBUFQUEUE / MEMORY SEPARATION</div><h1>把积压留在后台，<br>让写线程使用小数据池</h1><p>缓存敏感业务与 32 B 消息写入交替执行。回调暂停期间，两层结构继续复制和回收第一层，后台保留待处理消息。</p><span class='badge'>Apple M3</span><span class='badge'>3 次独立运行取平均</span><span class='badge'>高 QoS · 未硬绑核</span><span class='badge'>每条消息校验</span></div>
<div class='cards'><div class='card'><div class='muted'>TwoBufQueue 写线程侧数据池</div><div class='number'>{hot}</div><p>另有固定 256 MiB 后台池</p></div><div class='card'><div class='muted'>写线程侧数据区上限对比</div><div class='number'>约 {shrink:,.0f}× 更小</div><p>开源对照的数据区约 256 MiB</p></div><div class='card'><div class='muted'>主场景 · 对本次最快开源对照</div><div class='number'>{h}</div><p>{fmt(tb['throughput'])} M 次/s；对照 {esc(LABELS[fastest['case']['parameters']['transport']])}</p></div></div>
<section><h2>优势在哪里：后台积压不要求扩大第一层</h2><div class='pipeline'><div><b>TwoBufQueue</b><p>业务线程 → <span class='chip'>{hot} 第一层</span><br>复制线程 → <span class='chip queue'>256 MiB 第二层</span> → 回调线程</p></div><div><b>原生 SPSC 对照</b><p>业务线程 → <span class='chip queue'>约 256 MiB 队列数据区</span><br>消费线程 → 回调</p></div></div><p>每轮最多写入 6,710,886 条、约 204.8 MiB。主场景暂停回调，写线程先完成本轮，随后放开回调并排空。全部实现使用相同消息条数、32 B 数据和业务逻辑。</p><p><b>在放开回调前的统计点，TwoBufQueue 已把约 {copied_percent:.1f}% 的正式消息字节复制到第二层；{tb['pause_confirmed_rounds']} / {tb['completed_rounds']} 个正式轮次确认回调处于暂停状态。</b>数据积压在后台，第一层继续循环复用。</p><p class='note'>“写线程侧数据区”是写线程会使用的队列数据缓冲上限，<b>不等于 L1 占用、实际活跃缓存行或进程 RSS</b>。TwoBufQueue 总数据池为 256 MiB + {hot}，原生队列约 256 MiB；框架、块描述符和业务工作集另计。</p></section>
<section><h2>主展示：可控后台积压</h2><p>私有业务工作集 96 KiB，每次执行 8 次依赖读取与更新，之后写入一条 32 B 消息。绿色整行是 TwoBufQueue。</p>{table(main)}<p class='note'><b>延时口径：</b>计时组包含连续 1,000 次“业务计算＋消息生成＋队列写入”，用组耗时 ÷ 1,000 得到 ns/次，再统计 p50/p99/p999。它是组内平均耗时的分位数，<b>不是单次 write 延时，也不是消息到达回调的延时</b>。吞吐使用独立无逐条计时的运行；轮末交接与排空不计入热路径时间。</p></section>
"""
    body+="<section><details><summary>主场景的三次独立吞吐值</summary><div class='scroll'><table><thead><tr><th>实现</th><th>第1次 M次/s</th><th>第2次 M次/s</th><th>第3次 M次/s</th><th>变异系数</th></tr></thead><tbody>"
    for r in main:
        t=r['case']['parameters']['transport']; cls='tb' if t=='two_buf_queue' else ''
        body+=f"<tr class='{cls}'><th>{LABELS[t]}</th>"+''.join(f'<td>{fmt(v)}</td>' for v in r['throughput_repeats'])+f"<td>{r['throughput_cv_percent']:.2f}%</td></tr>"
    body+="</tbody></table></div></details></section>"
    for scenario in ['slow','fast']:
        desc='所有队列每条回调执行同一个 16 次计算循环；复制与回调在 TwoBufQueue 中仍分属两个线程。' if scenario=='slow' else '回调计算循环次数为 0；条数、FIFO 和消息内容校验仍执行。'
        body+=f"<section><h2>补充对照：{SCENARIOS[scenario]}</h2><p>{desc}</p>{table(summary['main'][scenario])}</section>"
    body+="<section><h2>第一层变化，第二层保持 256 MiB</h2><p>同一主场景，普通 write，完整块交接；源池固定 4 块。以下所有候选都保留，主展示取四项加权成绩与最高值相差不超过 2% 的候选中最小的写侧池，并要求相邻候选支持；筛选后另做三次独立对比。</p><div class='scroll'><table><thead><tr><th>Writer 块</th><th>写侧池</th><th>Reader 块</th><th>业务吞吐 M次/s</th><th>p50 ns/次</th><th>p99 ns/次</th><th>p999 ns/次</th><th>背压%</th></tr></thead><tbody>"
    for r in sorted(summary['source_sweep'],key=lambda r:r['source_bytes']):
        q=r['queue_config'];cls='tb' if r['case']['id']==summary['selected_twobuf']['case']['id'] else ''
        body+=f"<tr class='{cls}'><th>{q['source_block_size']//1024:,} KiB</th><td>{r['source_bytes']//1024:,} KiB</td><td>{q['output_block_size']//1024:,} KiB</td><td>{fmt(r['throughput'])}</td><td>{fmt(r['p50'])}</td><td>{fmt(r['p99'])}</td><td>{fmt(r['p999'])}</td><td>{r['backpressured_messages']/r['messages']*100:.0f}%</td></tr>"
    body+="</tbody></table></div></section>"
    body+=f"<section><h2>纯计算对照</h2><p>保持相同消息与后台暂停，仅把私有表读取改为 8 次整数计算。原生队列的复制选项在纯计算负载下另行复测后选择吞吐较好的选项，避免复用缓存敏感场景的选项造成失真。这个对照不能用于证明业务数据的缓存保护收益。</p>{table(summary['arithmetic'])}</section>"
    body+="<section><details><summary>纯计算的复制选项与初始化参数</summary><div class='scroll'><table><thead><tr><th>实现 / 复制选项</th><th>业务吞吐 M次/s</th><th>p50 ns/次</th><th>p99 ns/次</th><th>p999 ns/次</th></tr></thead><tbody>"
    for r in summary['arithmetic_variants']:
        t=r['case']['parameters']['transport'];q=r['queue_config']
        variant=f"zero_then_copy={q['readerwriterqueue_copy_after_zero']}" if t=='readerwriterqueue' else f"write_prefetch={q['rigtorp_write_prefetch']}"
        body+=f"<tr><th>{LABELS[t]} · {variant}</th><td>{fmt(r['throughput'])}</td><td>{fmt(r['p50'])}</td><td>{fmt(r['p99'])}</td><td>{fmt(r['p999'])}</td></tr>"
    body+="</tbody></table></div>"
    for r in summary['arithmetic']:
        t=r['case']['parameters']['transport']
        body+=f"<details><summary>{LABELS[t]} · 纯计算完整参数</summary><pre>{esc(json.dumps({'business_kernel':'arithmetic','business_accesses':8,'effective_business_table_bytes':0,'queue':r['queue_config']},ensure_ascii=False,indent=2))}</pre></details>"
    body+="</details></section>"
    body+="<section><h2>测试条件与初始化参数</h2><p>正式每项 1,000 万条，3 次独立进程取算术平均；运行顺序打乱，测试串行执行。固定 256 MiB 第二层，每轮上限为其 80%，不随第一层变化。队列页通过真实管道预热两轮，业务预热 20,000 次。首次分配和预热不计时。</p><p>普通 write：publish_write_ptr_count = UINT32_MAX、is_scan_current = false；消息不写长度。源池与输出池在构造时预分配，块满后交接；已写消息在完整块或轮末交接后供复制线程读取。原生队列使用 spsc_* 字段，初始化记录中的 source/output 字段只供公共配置校验，不会构建双层管道。原生队列保留上游源码与原生入队接口，复制选项在这个相同业务负载下复测，选出三次平均吞吐较好的选项，三个缓存敏感回调场景保持该选项；纯计算负载另外筛选原生复制选项。</p>"
    body+=f"<details><summary>机器、编译器与电源记录</summary><pre>{esc(json.dumps(summary['environment'],ensure_ascii=False,indent=2))}</pre></details>"
    for r in summary['main']['paused']:
        t=r['case']['parameters']['transport']
        body+=f"<details><summary>{esc(LABELS[t])} · 完整初始化参数</summary><pre>{esc(json.dumps({'business':{'bytes':96*1024,'accesses':8,'entry_bytes':64,'warmup_steps':20000,'seed':42},'scenarios':{'paused':{'gate':'hold first callback until round writer completes'},'slow':{'callback_iterations':16},'fast':{'callback_iterations':0}},'queue':r['queue_config']},ensure_ascii=False,indent=2))}</pre></details>"
    body+="<details><summary>开源复制选项对照（全部保留）</summary><div class='scroll'><table><thead><tr><th>实现 / 复制选项</th><th>业务吞吐 M次/s</th><th>p50 ns/次</th><th>p99 ns/次</th><th>p999 ns/次</th></tr></thead><tbody>"
    for r in summary['native_variants']:
        q=r['queue_config'];t=r['case']['parameters']['transport'];copy=(f"zero_then_copy={q['readerwriterqueue_copy_after_zero']}" if t=='readerwriterqueue' else f"write_prefetch={q['rigtorp_write_prefetch']}")
        body+=f"<tr><th>{LABELS[t]} · {copy}</th><td>{fmt(r['throughput'])}</td><td>{fmt(r['p50'])}</td><td>{fmt(r['p99'])}</td><td>{fmt(r['p999'])}</td></tr>"
    body+="</tbody></table></div></details></section>"
    n=summary['audit'];body+=f"<section><h2>数据与校验</h2><p>{n['measurement_runs']} 项正式运行、{n['validation_runs']} 项独立全负载校验，全部通过。独立校验逐条比较完整 32 B 业务结果；正式运行核验 FIFO、每条消息的确定字段与整体校验和。每条正式消息均完成回调后才结束该测试。</p><p>原始 JSON 包含每次结果、全部参数、精确命令、可执行文件签名、背压条数、轮次、回调时长、页错误和数据校验记录。整数背压%会显示为 0% 的极小比例，表内同时列出精确条数。</p><p class='note'>结果仅描述这台 M3 上的合成业务与已列明的线程调度条件。主场景主动暂停回调，用于展示积压与写侧数据池的解耦；慢回调、即时消费和纯计算对照一并列出。当前缓存敏感主场景的吞吐优势较小，补充场景还有落后指标；这份测试证明小写侧池与大后台积压可以解耦，尚未证明能显著减少业务缓存干扰。未采集新的硬件 PMU 数据，不将速度差异直接归因于 L1 命中率。</p><p><a href='{data_href}/summary.json'>汇总 JSON</a> · <a href='{data_href}/results.csv'>全部汇总 CSV</a> · <a href='{data_href}/manifest.json'>复现命令与文件清单</a></p></section><p class='muted'>生成于 {esc(summary['generated_at'])} · 性能指标来自真实运行，容量优势指队列写侧数据区。</p></main></html>"
    report_path.write_text(body)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--environment',type=Path)
    parser.add_argument('--render-only',action='store_true')
    parser.add_argument('--report',type=Path,help='HTML output path; defaults to docs/performance_hot_path_cache.html for the documented directory layout')
    a=parser.parse_args();exe=a.executable.resolve();directory=a.output.resolve()
    directory.mkdir(parents=True,exist_ok=True);(directory/'raw').mkdir(exist_ok=True)
    if a.render_only:
        html_report(directory,json.loads((directory/'summary.json').read_text()),a.report);return
    first=[]
    def add(identifier,params,category):
        c={'id':identifier,'parameters':params,'category':category};first.append(c);return c
    baseline=add('baseline',{'transport':'baseline','scenario':'paused'},'baseline')
    sources=[add(f'twobuf_{b}k',{'transport':'two_buf_queue','scenario':'paused',**blocks(b)},'source_sweep') for b in [8,16,32,64,128,512,2048]]
    variants=[]
    for t in ['readerwriterqueue','rigtorp']:
        for alt in [False,True]:variants.append(add(t+('_alternative' if alt else '_default'),{'transport':t,'scenario':'paused',**native(t,alt)},'native_copy_control'))
    atom=add('atomic_queue',{'transport':'atomic_queue','scenario':'paused',**native('atomic_queue')},'native')
    (directory/'plan_stage1.json').write_text(json.dumps({'defaults':common(),'repetitions':3,'cases':first},indent=2))
    measure(exe,directory,first)
    source_rows=[aggregate(directory,c) for c in sources];selected=score_pick(source_rows)
    native_rows=[aggregate(directory,c) for c in variants]
    picked=[selected['case']]
    for t in ['readerwriterqueue','rigtorp']:
        picked.append(max([r for r in native_rows if r['case']['parameters']['transport']==t],key=lambda r:r['throughput'])['case'])
    picked.append(atom)
    previous = directory/'exploratory_peak_plan.json'
    old_plan = directory/'plan_stage2.json'
    if not previous.exists() and old_plan.exists():
        previous.write_text(old_plan.read_text())
    extra_cases = json.loads(previous.read_text())['cases'] if previous.exists() else []
    confirmed_baseline = {'id':'showcase_baseline','parameters':baseline['parameters'],'category':'confirmed_baseline'}
    confirmed_paused = [{'id':'showcase_'+c['id']+'_paused','parameters':c['parameters'],'category':'confirmed_paused'} for c in picked]
    later=[confirmed_baseline]+confirmed_paused
    for scenario in ['slow','fast']:
        for c in picked:
            later.append({'id':'showcase_'+c['id']+'_'+scenario,'parameters':{**c['parameters'],'scenario':scenario,'callback_iterations':16 if scenario=='slow' else 0},'category':scenario})
    for c in [baseline]+picked:
        later.append({'id':'showcase_'+c['id']+'_arithmetic','parameters':{**c['parameters'],'business_kernel':'arithmetic'},'category':'arithmetic'})
    (directory/'plan_stage2.json').write_text(json.dumps({'defaults':common(),'repetitions':3,'selected_source_case':selected['case']['id'],'cases':later},indent=2))
    measure(exe,directory,later)
    arithmetic_controls=[]
    for t in ['readerwriterqueue','rigtorp']:
        base=next(c for c in picked if c['parameters']['transport']==t)
        for alt in [False,True]:
            arithmetic_controls.append({'id':'arithmetic_copy_'+t+('_alternative' if alt else '_default'),
                'parameters':{**base['parameters'],**native(t,alt),'business_kernel':'arithmetic'},
                'category':'arithmetic_copy_control'})
    (directory/'plan_arithmetic_copy_control.json').write_text(json.dumps({'defaults':common(),'repetitions':3,'cases':arithmetic_controls},indent=2))
    measure(exe,directory,arithmetic_controls)
    arithmetic_variant_rows=[aggregate(directory,c) for c in arithmetic_controls]
    arithmetic_rows=[aggregate(directory,c) for c in later if c['category']=='arithmetic' and
        c['parameters']['transport'] not in ['readerwriterqueue','rigtorp']]
    for t in ['readerwriterqueue','rigtorp']:
        arithmetic_rows.append(max((r for r in arithmetic_variant_rows if r['case']['parameters']['transport']==t),key=lambda r:r['throughput']))
    order=['baseline','two_buf_queue','readerwriterqueue','rigtorp','atomic_queue']
    arithmetic_rows.sort(key=lambda r:order.index(r['case']['parameters']['transport']))
    b=aggregate(directory,confirmed_baseline)
    all_cases=first+later+arithmetic_controls+[c for c in extra_cases if c['id'] not in {v['id'] for v in first+later}]
    summary={'generated_at':time.strftime('%Y-%m-%d %H:%M:%S %z'),'environment':json.loads(a.environment.read_text()) if a.environment else {'platform':platform.platform()},
             'source_sweep':source_rows,'selected_twobuf':selected,'native_variants':native_rows,
             'main':{'paused':[b]+[aggregate(directory,c) for c in confirmed_paused]},
             'arithmetic':arithmetic_rows,'arithmetic_variants':arithmetic_variant_rows}
    for scenario in ['slow','fast']:
        summary['main'][scenario]=[b]+[aggregate(directory,c) for c in later if c['category']==scenario]
    for scenario,rows in summary['main'].items():
        if len({r['business_checksum'] for r in rows})!=1:raise RuntimeError('cross-library business checksum mismatch')
    if len({r['business_checksum'] for r in summary['arithmetic']}) != 1:
        raise RuntimeError('arithmetic cross-library business checksum mismatch')
    summary['audit']={'measurement_runs':len(all_cases)*6,'validation_runs':len(all_cases),'failed_runs':0}
    (directory/'summary.json').write_text(json.dumps(summary,indent=2,ensure_ascii=False)+'\n')
    with (directory/'results.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=['case','transport','scenario','kernel','writer_block_bytes','writer_pool_bytes','reader_block_bytes','reader_pool_bytes','throughput_m_iterations_s','p50_group_ns','p99_group_ns','p999_group_ns','backpressured_messages','messages','throughput_cv_percent'])
        writer.writeheader()
        for c in all_cases:
            r=aggregate(directory,c);q=r['queue_config'];writer.writerow({'case':c['id'],'transport':c['parameters']['transport'],'scenario':c['parameters']['scenario'],'kernel':c['parameters'].get('business_kernel','cache_sensitive'),'writer_block_bytes':q['source_block_size'],'writer_pool_bytes':r['source_bytes'],'reader_block_bytes':q['output_block_size'],'reader_pool_bytes':q['output_block_size']*q['output_pool_max'],'throughput_m_iterations_s':r['throughput'],'p50_group_ns':r['p50'],'p99_group_ns':r['p99'],'p999_group_ns':r['p999'],'backpressured_messages':r['backpressured_messages'],'messages':r['messages'],'throughput_cv_percent':r['throughput_cv_percent']})
    manifest={'executable_sha256':sha(exe),'source_sha256':{p.name:sha(p) for p in [Path(__file__),Path(__file__).with_name('hot_path_cache.hpp'),Path(__file__).with_name('test_hot_path_cache.cpp')]},
              'upstream_manifest':json.loads(Path(__file__).with_name('spsc_dependencies.json').read_text()),
              'reproduce':['python3','benchmarks/test_hot_path_cache.py','--executable','build/test_hot_path_cache','--output','docs/performance_data/hot_path_cache_reproduced','--report','docs/performance_hot_path_cache_reproduced.html'],
              'raw_files':[{'path':str(p.relative_to(directory)),'sha256':sha(p)} for p in sorted((directory/'raw').glob('*.json'))],
              'selection':'TwoBufQueue: .35 throughput + .30 p50 + .20 p99 + .15 p999, best-direction ratio normalization within source candidates; adjacent-candidate support; smallest source pool within 2% of peak score. Native: fastest three-run mean throughput of original copy options, independently selected for cache-sensitive and arithmetic workloads. Independent final confirmation; fixed parameters across cache-sensitive callback scenarios.'}
    (directory/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    html_report(directory,summary,a.report)
    for scenario,rows in summary['main'].items():
        print(scenario,[(r['case']['id'],round(r['throughput'],3),round(r['p50'],3)) for r in rows],flush=True)
if __name__=='__main__':main()
