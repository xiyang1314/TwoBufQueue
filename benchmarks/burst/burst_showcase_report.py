# -*- coding: utf-8 -*-
"""Single-metric report: business cost under a large message backlog."""
import html
import json
import os
import statistics
from pathlib import Path

QUEUES = ('TwoBufQueue', 'ReaderWriterQueue', 'rigtorp', 'atomic_queue')
NAMES = {'TwoBufQueue': 'TwoBufQueue', 'ReaderWriterQueue': 'ReaderWriterQueue',
         'rigtorp': 'rigtorp::SPSCQueue', 'atomic_queue': 'atomic_queue'}


PALETTES = (
    ('#eaf3fc', '#b9d4ee', '#246ca7'),
    ('#f1ecfa', '#d0c1ec', '#7653a4'),
    ('#e9f5ee', '#b9ddc9', '#2b7c5d'),
    ('#fff3df', '#ead1a4', '#a7681d'),
    ('#e8f4f6', '#b6dce1', '#247580'),
)


def group_style(index):
    background, line, accent = PALETTES[index % len(PALETTES)]
    return f'--group-bg:{background};--group-line:{line};--group-accent:{accent}'


def comparison_cards(bursts, rows):
    h = '<p class="chart-note">每组对比四个队列，柱条越短表示业务耗时越低。每张图按本组最大耗时缩放，精确数值标在右侧，单位为 ns/次。</p><div class="metrics-grid" aria-label="各发送量的业务耗时对比">'
    for index, burst in enumerate(bursts):
        maximum = max(rows[queue, burst]['p50'] for queue in QUEUES) or 1.0
        h += f'<article class="mini-card" data-burst="{burst}" style="{group_style(index)}"><div class="card-head"><span class="badge">每批 {burst} MiB</span><span class="card-meta">业务耗时 · p50</span></div>'
        for queue in QUEUES:
            value = rows[queue, burst]['p50']
            width = value / maximum * 100
            name = html.escape(NAMES[queue])
            classes = 'bar-row is-tbq' if queue == 'TwoBufQueue' else 'bar-row'
            h += f'<div class="{classes}" data-queue="{queue}" data-cost="{value:.1f}"><span>{name}</span><div class="bar-track" aria-hidden="true"><div class="bar" style="width:{width:.2f}%"></div></div><span class="bar-value">{value:.1f}</span></div>'
        h += '</article>'
    return h + '</div>'


def render_report(folder, report, result):
    profile = result['parameters']
    # Show the four standard burst sizes; keep isolated diagnostic datasets renderable.
    bursts = [b for b in profile['bursts_mib'] if b in (2, 4, 8, 16)] or list(profile['bursts_mib'])
    business = profile['selected_business_kib']
    q = profile.get('queue_parameters', {}).get('TwoBufQueue', {})
    source_count = q.get('source_pool_max', 4)
    source_kib = q.get('source_block_size', 4096) // 1024
    output_count = q.get('output_pool_max', 65536)
    output_kib = q.get('output_block_size', 4096) // 1024
    output_mib = profile.get('reader_pool_total_bytes', 256 * 1048576) / 1048576
    target_percent = profile.get('benchmark_parameters', {}).get('target_percent', 80)
    target_mib = output_mib * target_percent / 100
    source_total_kib = profile.get('writer_pool_total_bytes', 16384) // 1024
    by = {(r['queue'], r['burst_mib']): r for r in result['rows']}
    baseline = [r['p50'] for r in result['rows'] if r['queue'] == 'baseline' and r['burst_mib'] in bursts]
    baseline_ns = statistics.mean(baseline) if baseline else None
    occupancy = [run['occupancy_mean'] * output_mib
                 for row in result['rows'] if row['queue'] in QUEUES and row['burst_mib'] in bursts
                 for run in row.get('per_run', []) if 'occupancy_mean' in run]
    parameters_view = {key: value for key, value in profile.items() if key != 'bursts_mib'}
    parameters_view['display_bursts_mib'] = bursts
    prefix = os.path.relpath(folder, report.parent).replace(os.sep, '/')
    profile_reference = os.path.relpath(folder/'selected_config.json', Path(__file__).resolve().parents[2]).replace(os.sep, '/')
    burst_list = '、'.join(str(b) for b in bursts) + ' MiB'
    machine = dict(profile.get('machine', {}))
    environment_file = folder / 'environment.json'
    if environment_file.exists():
        machine.update(json.loads(environment_file.read_text()).get('machine', {}))
    cpu = html.escape(machine.get('cpu', '测试机器'))
    scheduling = ('高 QoS · 未硬绑核'
                  if machine.get('producer_qos') == 'user_interactive'
                  else '未硬绑核')
    h = '''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>TwoBufQueue · 消息积压对热路径业务的影响</title><style>
:root{--ink:#183048;--sub:#576d83;--green:#087f6e;--tb:#dff4ee}*{box-sizing:border-box}body{margin:0;background:#f3f6fb;color:var(--ink);font:16px/1.8 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}main{max-width:1000px;margin:auto;padding:30px 22px}h1{font-size:34px;line-height:1.35;margin:0 0 12px}h2{font-size:22px;margin:0 0 14px}section{background:white;border:1px solid #dce5ee;border-radius:16px;padding:24px;margin:20px 0}p{margin:12px 0}.hero{background:#102d40;color:white}.hero p{color:#c3d9e5}.tag{border:1px solid #486272;border-radius:20px;padding:3px 9px;display:inline-block;font-size:12px;margin-right:6px}.steps{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin:16px 0}.steps span{padding:12px 18px;border-radius:10px;background:#edf3f8;font-weight:700}.steps b{color:#648299}.settings{display:grid;grid-template-columns:repeat(3,1fr);gap:14px}.settings div{padding:15px;background:#f0f5fa;border-radius:10px}.settings strong{display:block;color:var(--green);font-size:23px}.baseline{background:#edf6ff;border-left:4px solid #315f86;padding:14px 18px;font-size:19px;font-weight:750}.scroll{overflow:auto}table{width:100%;max-width:640px;min-width:530px;margin:0 auto;border:1px solid #afc2d1;border-collapse:collapse;table-layout:fixed;font-variant-numeric:tabular-nums;font-size:15px}col.volume-col{width:23%}col.queue-col{width:47%}col.cost-col{width:30%}th,td{padding:12px 18px;border:1px solid #bfceda;white-space:nowrap}thead th{background:#edf2f8;color:#405870;font-size:14px}th.volume-heading{text-align:center}th.queue-heading,th.queue{text-align:right}th.cost,td.cost{text-align:left;padding-left:20px;font-weight:750}.metric-value{display:inline-block;min-width:5ch;text-align:right}.burst-group{--group-bg:#eaf3fc;--group-accent:#246ca7}.burst-group>tr:first-child>th,.burst-group>tr:first-child>td{border-top:2px solid #91aabe}.burst-group .volume-label{background:var(--group-bg);color:var(--group-accent);text-align:center;vertical-align:middle;border-right:2px solid var(--group-accent);font-size:18px;font-weight:750}.volume-label small{display:block;color:#576d83;font-size:11px;font-weight:400;margin-top:6px}.burst-group .queue,.burst-group .cost{background:#fff}.burst-group .queue{font-weight:500}.burst-group .tb .queue,.burst-group .tb .cost{background:var(--tb);color:#075c4e;font-weight:750}.metrics-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px;margin:20px 0 26px}.mini-card{border:1px solid var(--group-line);background:var(--group-bg);border-radius:10px;padding:17px 18px}.mini-card:last-child:nth-child(odd){grid-column:1/-1}.card-head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:16px}.badge{font-size:13px;background:var(--group-accent);color:white;padding:4px 10px;border-radius:5px;font-weight:750;white-space:nowrap}.card-meta{color:#576d83;font-size:12px}.bar-row{display:grid;grid-template-columns:145px minmax(32px,1fr) 52px;gap:10px;align-items:center;font-size:12px;margin-top:13px}.bar-track{height:9px;background:#ffffffbf;border-radius:5px;overflow:hidden}.bar{height:9px;background:#91a9bc;border-radius:5px}.bar-row.is-tbq{font-weight:750;color:#075c4e}.bar-row.is-tbq .bar{background:#087f6e}.bar-value{text-align:right;font-variant-numeric:tabular-nums}.chart-note{font-size:13px;color:#576d83}.comparison-title{font-size:17px;margin:22px 0 14px}@media(max-width:650px){.metrics-grid{grid-template-columns:1fr}.mini-card:last-child:nth-child(odd){grid-column:auto}.bar-row{grid-template-columns:132px minmax(28px,1fr) 44px;gap:8px}}.muted,small{color:var(--sub)}summary{font-weight:700;cursor:pointer}details{margin:14px 0}dt{font-weight:700;margin-top:14px}dd{margin:4px 0 12px}#test-environment dl{display:grid;grid-template-columns:145px minmax(0,1fr);gap:10px 18px;margin:0}#test-environment dt,#test-environment dd{margin:0}#test-environment dd{overflow-wrap:anywhere}pre{background:#f3f6fa;padding:14px;font-size:12px;overflow:auto;max-height:420px;line-height:1.5}a{color:var(--green)}@media(max-width:650px){main{padding:14px}section{padding:18px}h1{font-size:27px}.settings{grid-template-columns:repeat(2,1fr)}}
</style></head><body><main>'''
    h += f'''<section class="hero"><h1>消息积压时，热路径业务还能跑多快？</h1><p>模拟消费较慢、队列已有大量待处理消息的场景，比较几种队列对同一热路径业务的影响。</p><span class="tag">{cpu}</span><span class="tag">三次独立运行</span><span class="tag">{scheduling}</span><span class="tag">全部消息校验</span></section>'''
    h += '''<section><h2>模拟场景</h2><p>在通常人们使用spsc队列的时候会遇到一个问题，就是时不时会出现消费者线程因为突发原因而产生任务积压，从而拖累生产者线程。这时处理方式一般是加大队列的缓存容量。但是，缓存队列容量要分配多少才能避免这种偶然突发而造成的背压，这是个令人头痛的问题。而且即便扩大缓存容量，消息积压也会很大程度的影响生产者线程的l1、l2或者l3的缓存命中率，从而影响热路径性能。这对于比如机器人，高频交易等低延时的应用来说影响会比较大。而双层缓存能比较好的解决这类问题，这正是TwoBufQueue的设计思路。</p><p>这个测试模拟的是让消息队列产生消息积压，看看几种队列在消息积压时对业务产生的性能影响状况。</p></section>'''
    h += f'''<section><h2>测试流程</h2><div class="steps"><span>循环发送一批消息</span><b>→</b><span>立即执行业务函数</span><b>↻</b></div><p>业务函数访问并更新一张 {business} KiB 数据表，每次处理 8 个节点。本页展示每批发送量为 {burst_list} 的结果，各发送量均重复 {profile['final_window_repeats']:,} 次。复制、消费者线程持续工作，准备积压和最终排空的时间不计入业务耗时。</p></section>'''
    h += f'''<section><h2>基本参数</h2><div class="settings"><div>消息长度<strong>{profile.get('payload_bytes',32)} B</strong>全部字段随序号变化</div><div>每批发送量<strong>{min(bursts)}–{max(bursts)} MiB</strong>{burst_list}</div><div>业务数据表<strong>{business} KiB</strong>每次访问并更新 8 个节点</div><div>TwoBufQueue 第一层<strong>{source_total_kib} KiB</strong>{source_count} × {source_kib} KiB</div><div>第二层 / 开源槽位存储<strong>{output_mib:.0f} MiB</strong>队列元数据另计</div><div>待处理消息的积压目标<strong>约 {target_mib:.1f} MiB</strong>缓冲容量的 {target_percent}%</div></div>'''
    if occupancy:
        h += f'<p>各项发送结束时的平均实际积压约 <b>{min(occupancy):.0f}–{max(occupancy):.0f} MiB</b>。回调一直在消费消息，因此实际值会随发送量和处理速度变化。这里统计的是待处理消息的字节数。</p>'
    h += f'''<p>每条消息的回调统一执行 <b>{profile['callback_iterations']} 次</b>计算循环；每项每轮采集 <b>{profile['samples_per_run']:,} 组</b>业务耗时，独立运行 <b>{profile['runs']} 轮</b>。所有队列采用同样的消息内容和业务负载。</p></section>'''
    build = profile.get('build', {})
    hardware = ' · '.join(str(machine[k]) for k in ('name', 'model') if machine.get(k)) or '型号未记录'
    cores = str(machine.get('physical_cores', '未记录'))
    if machine.get('performance_cores') is not None and machine.get('efficiency_cores') is not None:
        cores += f" 核（{machine['performance_cores']} 性能核 + {machine['efficiency_cores']} 能效核）"
    elif machine.get('physical_cores') is not None:
        cores += ' 核'
    memory = f"{machine['memory_bytes'] / (1024**3):g} GiB" if machine.get('memory_bytes') else '未记录'
    os_name = str(machine.get('os', '未记录'))
    if machine.get('os_version'):
        os_name = f"macOS {machine['os_version']}"
        if machine.get('os_build'):
            os_name += f"（构建号 {machine['os_build']}）"
    compiler = ' '.join(str(build.get(k, '')) for k in ('CMAKE_CXX_COMPILER_ID', 'CMAKE_CXX_COMPILER_VERSION')).strip() or '未记录'
    optimization = ' '.join(str(build.get(k, '')) for k in ('CMAKE_CXX_FLAGS', 'CMAKE_CXX_FLAGS_RELEASE')).strip() or '未记录'
    environment_rows = [
        ('硬件', hardware),
        ('处理器', str(machine.get('cpu', '未记录')) + ' · ' + cores),
        ('内存', memory),
        ('操作系统', os_name + ' · ' + str(machine.get('architecture', 'arm64' if 'arm64' in str(machine.get('os', '')) else '架构未记录'))),
        ('编译器', compiler),
        ('构建与优化', 'C++20 · ' + str(build.get('CMAKE_BUILD_TYPE', '未记录')) + ' · ' + optimization),
        ('线程调度', scheduling + '；复制线程和消费者线程不作硬绑核'),
        ('计时方式', str(result.get('timer_name', '原始数据中记录')) + (f" · 计时刻度约 {result['timer_quantum_ns']:.1f} ns" if result.get('timer_quantum_ns') else '')),
        ('内存池深度统计', str(build.get('BRQ_ENABLE_POOL_DEPTH_STATS', '未记录'))),
    ]
    h += '<section id="test-environment"><h2>测试环境</h2><dl>'
    h += ''.join('<dt>' + html.escape(label) + '</dt><dd>' + html.escape(value) + '</dd>' for label, value in environment_rows)
    h += '</dl></section>'
    h += '<section><h2>性能测试</h2>'
    if baseline_ns is not None:
        h += f'<p class="baseline">无干扰时业务执行时间：约 {baseline_ns:.0f} ns/次</p>'
    h += '<p>表中<b>耗时</b>指发送消息后立即执行的业务函数耗时，采用 <b>p50</b>，反映常见执行成本；数值越小越好。每组计时包含 16 次业务调用，显示单位为 ns/次。</p>'
    h += comparison_cards(bursts, by)
    h += '<h3 class="comparison-title">业务耗时对比表</h3><div class="scroll"><table id="performance"><colgroup><col class="volume-col"><col class="queue-col"><col class="cost-col"></colgroup><thead><tr><th class="volume-heading" scope="col">每批发送量</th><th class="queue-heading" scope="col">队列</th><th class="cost" scope="col">耗时(ns/次)</th></tr></thead>'
    for index, burst in enumerate(bursts):
        h += f'<tbody class="burst-group" style="{group_style(index)}">'
        for row_index, queue in enumerate(QUEUES):
            r = by[queue, burst]
            h += f'<tr class="{"tb" if queue=="TwoBufQueue" else ""}">'
            if row_index == 0:
                messages = burst * 1048576 // 32
                h += f'<th class="volume-label" scope="rowgroup" rowspan="4">{burst} MiB<small>{messages:,} 条消息</small></th>'
            h += f'<th class="queue" scope="row">{NAMES[queue]}</th><td class="cost"><span class="metric-value">{r["p50"]:.1f}</span></td></tr>'
        h += '</tbody>'
    h += '</table></div><p class="muted">每轮独立统计业务计时组的 p50，再对三轮结果取平均。无干扰参考值来自同批无队列对照的 p50 平均值，显示为整数；精确数值保留在原始结果中。</p></section>'
    h += '<section><h2>参数明细与复现</h2><details><summary>测量方法与队列设置</summary><dl>'
    h += f'<dt>业务与采样</dt><dd>{business} KiB 数据表、64 B 节点、随机种子 42；所有实现调用同一个业务函数。每个发送窗口先预热业务数据，发送后立即执行 64 次业务，分成 4 组，每组 16 次。每项每轮 {profile["final_window_repeats"]:,} 个窗口，共 {profile["samples_per_run"]:,} 组。</dd>'
    h += f'<dt>积压准备</dt><dd>目标约 {target_mib:.1f} MiB 待处理消息。发送前预填充并为本批数据预留空间，复制线程及消费者线程持续运行。分配、预触碰缓冲页、准备积压和最终排空均在业务计时区之外。</dd>'
    h += f'<dt>TwoBufQueue</dt><dd>普通 write，不扫描当前块，publish_write_ptr_count=UINT32_MAX；第一层 {source_count} × {source_kib} KiB，第二层 {output_count:,} × {output_kib} KiB；复制线程 idle_spin_count={q.get("reader_spin_count",1000)}，重试 yield 间隔={q.get("retry_yield_every",1024)}。一个生产者线程、一个复制线程、一个消费者线程（执行回调）；尾块在业务计时结束后交接。临时写入失败时重试，全部消息最终处理完成。</dd>'
    h += '<dt>开源队列</dt><dd>ReaderWriterQueue 使用 try_enqueue 和普通成员复制；rigtorp::SPSCQueue 使用 try_push，不添加写预取；atomic_queue 使用 try_push 和 SPSC 模式。均采用常规用法，开源队列原实现未修改。</dd>'
    h += '<dt>容量口径</dt><dd>比较的是数据存储容量，元数据另计。ReaderWriterQueue 可用 8,372,224 个槽；rigtorp 请求 8,388,603 个槽，加备用槽和 padding 后为 256 MiB；atomic_queue 有 8,388,608 个槽，状态数组另计。TwoBufQueue 使用额外的复制线程。</dd>'
    h += '</dl></details><details><summary>复现命令与原始数据</summary><p>从项目根目录运行，自动构建、校准、遍历、正式采样并生成本格式报告：</p><pre>python3 benchmarks/burst/run_burst_showcase.py</pre>'
    h += f'<p>按本次固定参数重新执行：</p><pre>python3 benchmarks/burst/run_burst_showcase.py --replay {profile_reference}</pre><p>使用新的输出目录；中途停止后可传入原来的 --output 并加 --resume 继续。</p>'
    h += f'<p><a href="{prefix}/selected_config.json">全部初始化参数</a> · <a href="{prefix}/workflow.json">运行环境与代码哈希</a> · <a href="{prefix}/formal/report_results.json">完整结果和各轮原值</a></p><p>消息条数、FIFO、完整 32 B 内容及业务结果均通过校验。原始样本和完整统计保留，本页只展示业务 p50。</p><ul>'
    for queue in ('baseline',) + QUEUES:
        for run in range(1, profile['runs'] + 1):
            name = '无队列对照' if queue == 'baseline' else NAMES[queue]
            h += f'<li><a href="{prefix}/formal/raw/{business}k_{queue}_run{run}.json">{name} · 第 {run} 轮原始样本</a></li>'
    h += '</ul></details><details><summary>完整初始化参数与展示范围</summary><p>保留业务负载、各队列初始化、计时窗口和编译选项；display_bursts_mib 表示本页展示的发送量。原始测量参数与记录可通过上方链接查看。</p><pre>' + html.escape(json.dumps(parameters_view, ensure_ascii=False, indent=2)) + '</pre></details></section></main></body></html>'
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(h)
