# -*- coding: utf-8 -*-
"""Render one weighted, common configuration per library/mode for all lengths."""
import argparse
from collections import defaultdict
import hashlib
import html
import json
from pathlib import Path
import statistics

NAMES = {'two_buf_queue': 'TwoBufQueue', 'readerwriterqueue': 'ReaderWriterQueue',
         'rigtorp': 'rigtorp/SPSCQueue', 'atomic_queue': 'atomic_queue'}
MODES = {'session': 'WriteSession', 'publish_max': '普通 write · publish MAX',
         'publish1': '普通 write · 每条 publish'}
OBJECTIVES = {'throughput': '吞吐', 'p50': 'p50', 'p99': 'p99', 'p999': 'p999'}
OBJECTIVES['common'] = '统一参数'
LENGTHS = (8, 16, 32, 64)
FILENAMES = {m: f'performance_256m_zero_{m}_plateau.html' for m in MODES}

def esc(v):
    return html.escape(str(v), quote=True)

def num(v, places=3):
    return f'{v:,.{places}f}'

def size(v):
    for factor, unit in ((1024 ** 3, 'GiB'), (1024 ** 2, 'MiB'), (1024, 'KiB')):
        if v >= factor and v % factor == 0:
            return f'{v // factor} {unit}'
    return f'{v:,} B'

def anchor(n, t, o=None):
    return f'params-{t}'

def merged_values(records):
    """Show a shared value once, preserving real payload-dependent inputs."""
    merged = {}
    lengths = sorted(records)
    fields = records[lengths[0]]
    assert all(set(r)==set(fields) for r in records.values())
    for key in fields:
        values = [records[n][key] for n in lengths]
        if all(v==values[0] for v in values):
            merged[key] = values[0]
        elif key=='payload_bytes':
            merged[key] = lengths
        else:
            merged[key] = {'by_payload_bytes': {str(n):records[n][key] for n in lengths}}
    return merged

def parameters_table(values):
    return '<div class="table-scroll"><table class="initialization-table"><thead><tr><th>参数</th><th>取值</th></tr></thead><tbody>'+''.join(
        f'<tr><td class="mono">{esc(k)}</td><td>{esc(v if isinstance(v,str) else json.dumps(v,ensure_ascii=False))}</td></tr>'
        for k,v in values.items())+'</tbody></table></div>'

def constructor_inputs(c):
    p = c['parameters']
    extra = dict(source_thread_id=0, read_layer1_thread_count=1, writer_queue_enable_stats=False,
                 message_prefix_bytes=0, writer_count=1, source_copy_thread_count=1 if p['transport']=='two_buf_queue' else 0,
                 callback_thread_count=1, pool_depth_instrumentation=False,
                 callback_signature='(thread_id, data, payload_bytes)', callback_business_iterations=0)
    if p['transport']=='two_buf_queue':
        extra.update(writer_type=f'OwnedWriter<Writer<{p["publish_count"]}, false, uint16_t, void>>',
                     writer_api='bounded single-block WriteSession' if p['use_write_session'] else 'Writer::write',
                     ordinary_write_readback=not p['use_write_session'], sync_context='void',
                     callback_wrapper='FixedRecordCallback<PayloadBytes, Sink>',
                     source_pool_payload_bytes=p['source_block_size']*p['source_pool_max'],
                     reader_pool_payload_bytes=p['output_block_size']*p['output_pool_max'])
    else:
        extra.update(unused_fields='source_* / output_* / reader_spin_count / rescan_* / is_scan_current / publish_count: native SPSC does not construct TwoBufQueue layers',
                     upstream_unmodified=True, requested_native_capacity=p['spsc_capacity'],
                     native_template='ReaderWriterQueue<Payload,512>' if p['transport']=='readerwriterqueue' else
                         'rigtorp::SPSCQueue<Payload>' if p['transport']=='rigtorp' else
                         'AtomicQueueB2<Payload,std::allocator<Payload>,true,false,true>',
                     copy_policy='zero initialization then full memcpy' if p['readerwriterqueue_copy_after_zero'] and p['transport']=='readerwriterqueue' else
                         'member initialization copy + destination write prefetch' if p['transport']=='rigtorp' and p['rigtorp_write_prefetch'] else 'full member copy',
                     consumer_operation='peek → callback → pop' if p['transport']=='readerwriterqueue' else
                         'front → callback → pop' if p['transport']=='rigtorp' else 'try_pop → callback')
    return dict(performance_test_constructor_parameters=p, fixed_constructor_and_template_parameters=extra)

def copy_note(p):
    t = p['transport']
    if t == 'two_buf_queue':
        return '单块 WriteSession' if p['use_write_session'] else '普通 write · 首字节读回'
    if t == 'readerwriterqueue' and p['readerwriterqueue_copy_after_zero']:
        return '先清零，再完整 memcpy'
    if t == 'rigtorp' and p['rigtorp_write_prefetch']:
        return '成员复制＋目标写预取'
    return '完整成员复制'

EXTRA_CSS = '''
.param-cell{font-size:12px}.param-link{color:inherit;text-decoration:underline;text-decoration-color:#9aadbf;text-underline-offset:3px}
.param-group{margin:0 24px 18px;border:1px solid var(--group-line);border-radius:9px;background:var(--group-bg);overflow:hidden}
.param-group>summary{background:var(--group-bg)}.param-group .params{margin:10px 16px}.parameter-table td{background:var(--group-bg);font-weight:400}
.parameter-table th{font-size:11px}.parameter-table th:first-child,.parameter-table td:first-child{padding-left:16px}
.parameter-table tr:target{outline:3px solid var(--group-accent);outline-offset:-3px}.parameter-table td:last-child{text-align:left;white-space:normal;min-width:210px}
.missing{color:#925a12;font-weight:750}.params-title{display:flex;justify-content:space-between;align-items:center;gap:10px}.params-actions{padding:0 24px 18px}
.status-line{padding:0 24px 18px;font-size:12px;color:var(--muted)}.matrix table{min-width:820px}
.initialization-table{white-space:normal}.initialization-table th,.initialization-table td{text-align:left}.initialization-table td:first-child{width:34%;overflow-wrap:anywhere}.initialization-table td:last-child{overflow-wrap:anywhere;font:12px/1.6 ui-monospace,SFMono-Regular,Menlo,monospace}
.params>h3{padding:18px 16px 0}.params:target{outline:2px solid #246ca7;scroll-margin-top:74px}
.wide-note{max-width:1080px}.method-box ul{padding-left:20px}.method-box li{margin:5px 0}
@media print{.matrix table{min-width:0}table{white-space:normal}.params-actions{display:none}.params pre{max-height:none}}
'''

SCRIPT = r'''
document.querySelectorAll('[data-filter]').forEach(b=>b.addEventListener('click',()=>{
 document.querySelectorAll('[data-filter]').forEach(x=>x.setAttribute('aria-pressed',x===b?'true':'false'));
 document.querySelectorAll('[data-payload]').forEach(x=>x.hidden=b.dataset.filter!=='all'&&x.dataset.payload!==b.dataset.filter);
}));
document.querySelectorAll('a[href^="#params-"]').forEach(a=>a.addEventListener('click',()=>{
 const target=document.getElementById(a.getAttribute('href').slice(1));
 if(target){if(target.tagName==='DETAILS')target.open=true;let parent=target.parentElement;while(parent){if(parent.tagName==='DETAILS')parent.open=true;parent=parent.parentElement;}}
}));
document.getElementById('expand-params').addEventListener('click',()=>document.querySelectorAll('#parameters details').forEach(d=>d.open=true));
document.getElementById('collapse-params').addEventListener('click',()=>document.querySelectorAll('#parameters details').forEach(d=>d.open=false));
document.getElementById('export-json').addEventListener('click',()=>{
 const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([document.getElementById('report-data').textContent],{type:'application/json'}));
 a.download=document.body.dataset.mode+'_comparison.json';a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);
});
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=Path(__file__).parent)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--css', type=Path, default=Path(__file__).with_name('restyle_legacy_css.css'))
    weighted_dir = Path(__file__).parent / 'weighted_common_config_20261012'
    parser.add_argument('--selection', type=Path, default=weighted_dir / 'weighted_common_selection.json')
    parser.add_argument('--extra', type=Path, default=weighted_dir / 'readerwriterqueue_combined')
    args = parser.parse_args()
    data_dir = args.data.resolve()
    output_dir = (args.output or data_dir.parent.parent).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    selection = json.loads(args.selection.read_text())
    assert not selection['missing_groups'], 'Every library/mode needs a shared weighted plateau.'
    audit = json.loads((data_dir / 'audit.json').read_text())
    env = json.loads((data_dir / 'environment.json').read_text())
    source_hashes = {f: hashlib.sha256((data_dir / f).read_bytes()).hexdigest()
                     for f in ('comparison_results.json', 'raw.jsonl', 'audit.json', 'environment.json', 'selection_policy.json')}
    final_runs = defaultdict(list)
    for raw_file in (data_dir / 'raw.jsonl', args.extra / 'raw.jsonl'):
        with raw_file.open() as file:
            for line in file:
                row = json.loads(line)
                if row['stage'] == 'screen':
                    final_runs[row['candidate_id']].append(row)
    css = args.css.read_text() + EXTRA_CSS
    generated = []
    for mode, mode_name in MODES.items():
        selected_groups = {t:next(g for g in selection['groups'] if g['transport']==t and
                           g['writer_mode']==(mode if t=='two_buf_queue' else 'native')) for t in NAMES}
        report_rows = []
        for length in LENGTHS:
            for transport in NAMES:
                group = selected_groups[transport]
                chosen = group['selected']['center']['lengths'][str(length)]
                candidate = chosen['candidate']
                initialization = constructor_inputs(candidate)
                goals = {}
                for objective in ('throughput','p50','p99','p999'):
                    runs = sorted((r for r in final_runs[candidate['candidate_id']]
                                   if r['result']['config']['mode'] == ('throughput' if objective == 'throughput' else 'write_latency')),
                                  key=lambda r: r['trial'])
                    assert len(runs) == 3
                    values = [r['result']['m_messages_per_second'] if objective == 'throughput'
                              else r['result']['batch_average_write_ns'][objective] for r in runs]
                    assert abs(statistics.mean(values) - candidate['metrics'][objective]) < 1e-7
                    goals[objective] = {'confirmed_plateau': True, 'plateau_objective':'weighted_combination',
                                        'candidate_id': candidate['candidate_id'],
                                        'value': candidate['metrics'][objective], 'runs': values,
                                        'initialization': initialization,
                                        'measured_configs': [r['result']['config'] for r in runs],
                                        'neighborhood': None}
                tid = goals['throughput']['candidate_id']
                throughput_runs = sorted((r['result'] for r in final_runs[tid] if r['result']['config']['mode'] == 'throughput'),
                                         key=lambda r: r['config']['random_seed'])
                metrics = candidate['metrics']
                report_rows.append({'payload_bytes': length, 'transport': transport, 'goals': goals,
                                    'throughput_metrics': metrics, 'throughput_runs': throughput_runs,
                                    'weighted_payload_score':chosen['score'],
                                    'weighted_normalized_metrics':chosen['normalized'],
                                    'common_parameters':group['selected']['center']['common_parameters'],
                                    'weighted_total_score':group['selected']['center']['score'],
                                    'measurement_stage':'screen'})

        cards, performance, raw_tables, parameter_groups = [], [], [], []
        missing_names = []
        for length in LENGTHS:
            groups = [g for g in report_rows if g['payload_bytes'] == length]
            maximum = max(g['goals']['throughput']['value'] for g in groups)
            card = [f'<article class="mini-card group-{length}" data-payload="{length}"><span class="badge">{length} B</span><span class="card-meta">吞吐 · M条/s</span>']
            label = '回调循环 0 次 · 每次 10,000,000 条 · 三次平均'
            performance.append(f'<tbody class="group group-{length}" data-payload="{length}"><tr class="group-head"><th colspan="7"><span class="badge">{length} B</span><span class="group-meta">{label}</span></th></tr>')
            raw = [f'<details class="run-set group-{length}" data-payload="{length}"><summary><span class="badge">{length} B</span>三次原始结果与平均值</summary><div class="table-scroll"><table><thead><tr><th>实现 / 指标</th><th>第 1 次</th><th>第 2 次</th><th>第 3 次</th><th>平均</th></tr></thead><tbody>']
            for g in groups:
                t, goals, m = g['transport'], g['goals'], g['throughput_metrics']
                p = goals['throughput']['initialization']['performance_test_constructor_parameters']
                special = ' class="tbq"' if t == 'two_buf_queue' else ''
                throughput = goals['throughput']['value']
                values = goals['throughput']['runs']
                card.append(f'<div class="bar-row {"is-tbq" if t == "two_buf_queue" else ""}"><span>{NAMES[t]}</span><div class="bar-track"><div class="bar" style="width:{throughput / maximum * 100:.2f}%"></div></div><span class="bar-value">{num(throughput,2)}</span></div>')
                library = f'<td>{NAMES[t]}<span class="range">{esc(copy_note(p))}</span></td>'
                metric_cells = f'<td><a class="param-link" href="#{anchor(length,t,"throughput")}">{num(throughput)}</a><span class="range">{num(min(values))} – {num(max(values))}</span></td><td>{num(statistics.mean(r["payload_gb_per_second"] for r in g["throughput_runs"]))}</td>'
                for objective in ('p50','p99','p999'):
                    goal = goals[objective]
                    star = '<sup class="missing">*</sup>' if not goal['confirmed_plateau'] else ''
                    if star:
                        missing_names.append(f'{length} B · {NAMES[t]} · {objective}')
                    metric_cells += f'<td><a class="param-link" href="#{anchor(length,t,objective)}">{num(goal["value"])}</a>{star}</td>'
                backpressured = sum(r['backpressured_messages'] for r in g['throughput_runs'])
                total_messages = sum(r['config']['messages'] for r in g['throughput_runs'])
                percentage = backpressured / total_messages * 100
                bp_cell = f'<td class="backpressure-percent" data-backpressured="{backpressured}" data-messages="{total_messages}" title="三次吞吐测量合计：背压消息条数 / 发送总条数">{num(percentage,0)}%<span class="range">{backpressured:,} / {total_messages:,}</span></td>'
                performance.append(f'<tr data-library="{t}"{special}>{library}{metric_cells}{bp_cell}</tr>')
                for objective, goal in goals.items():
                    star = '*' if not goal['confirmed_plateau'] else ''
                    unit = 'M条/s' if objective == 'throughput' else 'ns/条'
                    raw.append(f'<tr{special}><td>{NAMES[t]}<span class="range"><a href="#{anchor(length,t,objective)}">{OBJECTIVES[objective]} {unit}{star} · 参数 {goal["candidate_id"]}</a></span></td>'+''.join(f'<td>{num(v)}</td>' for v in goal['runs'])+f'<td>{num(goal["value"])}</td></tr>')
            card.append('</article>');cards.append(''.join(card))
            performance.append('</tbody>')
            raw.append('</tbody></table></div></details>');raw_tables.append(''.join(raw))
        merged_initialization = {}
        for t in NAMES:
            rows = {g['payload_bytes']:g for g in report_rows if g['transport']==t}
            merged = merged_values({n:g['goals']['throughput']['initialization']['performance_test_constructor_parameters']
                                    for n,g in rows.items()})
            # Measurement mode and seed vary by run, not by message length.
            merged['mode'] = ['throughput','write_latency']
            merged['random_seed'] = sorted({p['random_seed'] for g in rows.values()
                                           for objective in ('throughput','p50')
                                           for p in g['goals'][objective]['measured_configs']})
            fixed = merged_values({n:g['goals']['throughput']['initialization']['fixed_constructor_and_template_parameters']
                                   for n,g in rows.items()})
            merged_initialization[t] = {'performance_test_constructor_parameters':merged,
                                        'fixed_constructor_and_template_parameters':fixed}
            parameter_groups.append(f'<details class="params" id="{anchor(None,t)}"><summary>{NAMES[t]}：所有消息长度共用的初始化参数</summary><p class="table-note">完整列出 61 项测试类参数及构造 / 模板参数。消息长度列为统一取值列表；容量条数和已固定的复制配置等随长度变化的值用 by_payload_bytes 对应关系表示，不重复四套参数。</p><h3>测试类参数</h3>{parameters_table(merged)}<h3>构造与模板参数</h3>{parameters_table(fixed)}</details>')

        mode_cfg = next(g for g in report_rows if g['transport']=='two_buf_queue')['goals']['throughput']['initialization']['performance_test_constructor_parameters']
        pub_label = 'MAX (4,294,967,295)' if mode_cfg['publish_count'] == 4294967295 else '1'
        scan_label = str(mode_cfg['is_scan_current']).lower()
        mode_note = '普通 write 在完整写入后读回首字节；Session 不加该读回。'
        unresolved = '；'.join(missing_names)
        warning = f'<div class="callout"><strong>* 未确认连续平台：</strong>{esc(unresolved)}。带 * 的单元格列出吞吐平台配置的该指标实测值，不能当作该指标的最优值。</div>' if missing_names else ''
        peers = ' · '.join(f'<a href="{FILENAMES[m]}">{esc(label)}</a>' for m,label in MODES.items() if m != mode)
        prefix = f'performance_data/{data_dir.name}/'
        weighted_prefix = 'weighted_common_config_20261012/'
        files = [(weighted_prefix+'weighted_common_selection.json','参数选择记录'),
                 (weighted_prefix+'select_weighted.py','选参程序'),
                 (weighted_prefix+'readerwriterqueue_combined/raw.jsonl','ReaderWriterQueue 补测记录'),
                 ('raw.jsonl','原始遍历记录'),('all_candidates.csv','原始遍历列表'),
                 ('audit.json','原始测量审计'),('environment.json','环境与依赖'),
                 ('measurement_sources.tar.gz','测量源码'),('restyle_comparison.py','报告生成器')]
        extra_audit = json.loads((args.extra / 'audit.json').read_text())
        compact_groups = {t:{**{k:g[k] for k in ('transport','writer_mode','references','qualified_plateaus','selected')},
                              'top_plateaus':[g['top_plateaus'][0]]} for t,g in selected_groups.items()}
        embedded = {'mode':mode,'label':mode_name,'rows':report_rows,'weighted_selection':compact_groups,
                    'weighting_policy':selection['policy'],'initialization_parameters_by_transport':merged_initialization,
                    'source_sha256':source_hashes,'audit':audit,'environment':env,
                    'additional_measurement_audit':extra_audit,
                    'same_configuration_all_metrics':True,'same_tunable_configuration_all_lengths':True}
        embedded_json = json.dumps(embedded,ensure_ascii=False).replace('<','\\u003c')
        title = f'256 MiB 缓存 · 回调循环为 0 · {mode_name}'
        content = f'''<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>{esc(title)} · SPSC 性能对比</title><style>{css}</style></head>
<body data-mode="{mode}"><header><div class="wrap"><div class="eyebrow">TWOBUFQUEUE / SPSC BENCHMARK</div><h1>{esc(title)}</h1>
<p>与 ReaderWriterQueue、rigtorp/SPSCQueue、atomic_queue 对比。按消息长度分组，所有结果来自已完成的同一批测量。</p>
<div class="chips"><span class="chip">每次 10,000,000 条</span><span class="chip">3 次平均</span><span class="chip">8 / 16 / 32 / 64 B 共用参数</span><span class="chip">权重 35 / 30 / 20 / 15</span><span class="chip">publish_count = {pub_label}</span><span class="chip">is_scan_current = {scan_label}</span><span class="chip">高 QoS · 未硬绑核</span></div></div></header>
<nav class="nav"><div class="wrap"><div class="nav-links"><a href="#overview">概览</a><a href="#performance">性能对比</a><a href="#runs">三次原始结果</a><a href="#parameters">全部参数</a><a href="#method">测试方法</a></div><div class="actions"><button id="export-json">导出 JSON</button><button onclick="window.print()">打印</button></div></div></nav>
<main class="wrap"><div class="footer">其他模式：{peers}</div>
<section id="overview"><div class="section-head"><h2>吞吐概览</h2><p class="sub">同一长度的四个队列放在一起；吞吐单位 M条/s。</p><div class="status"><span class="tag">完整数据与顺序校验通过</span><span class="tag">按四项指标选定共同参数</span><span class="tag">各队列每份报告只用一套调优参数</span></div></div>
<div class="intro"><p>TwoBufQueue 两层数据池各固定 <strong>256 MiB</strong>，合计 512 MiB；原生 SPSC 的消息存储预算为 256 MiB。备用槽、padding 计入预算，控制结构另计。</p><p><strong>每个队列的吞吐、p50、p99、p999 使用同一套参数，8 / 16 / 32 / 64 B 也共用这套调优参数。</strong>按权重 35% / 30% / 20% / 15% 综合选参，四种长度等权参与。原生容量随消息长度按固定字节预算推导，复制策略沿用各长度已固定的负载实现。</p><p>此次用共同参数对应的遍历阶段三次实测值更新表格，因此数值与旧版逐项独立选优不同；原始记录保留，ReaderWriterQueue 的邻域补测另外保存。</p></div><div class="metrics-grid">{''.join(cards)}</div></section>
<section id="performance" class="matrix"><div class="section-head"><h2>性能对比</h2><p class="sub">同消息长度使用相同背景色；TwoBufQueue 整行加粗并描边。吞吐下方显示三次测量的最小值与最大值。</p></div>
<div class="filter"><span>消息长度</span><button data-filter="all" aria-pressed="true">全部</button>{''.join(f'<button data-filter="{n}" aria-pressed="false">{n} B</button>' for n in LENGTHS)}</div>
<div class="table-scroll"><table id="comparison-table"><thead><tr><th>实现 / 复制方式</th><th>吞吐<br><span class="unit">M条/s</span></th><th>数据吞吐<br><span class="unit">GB/s</span></th><th>p50<br><span class="unit">ns/条</span></th><th>p99<br><span class="unit">ns/条</span></th><th>p999<br><span class="unit">ns/条</span></th><th>背压%</th></tr></thead>{''.join(performance)}</table></div>
<p class="table-note">延时是每组 <strong>1,000 次成功写入的平均耗时</strong>的分位数，再对三次运行取平均；并非单次调用或端到端延时。同一行所有指标来自同一配置；同一队列的四种长度也共用调优参数。综合选参是一套参数下的取舍，各单项不再声称分别最优。点击任一指标均跳到该行同一套完整参数。<strong>背压% = 背压消息条数 ÷ 总消息条数 × 100%</strong>，使用三次吞吐测量的合计数；下方同时列出分子 / 分母。每条消息即使写入失败多次，也只算一条背压消息。</p></section>
<section id="runs"><div class="section-head"><h2>三次原始结果</h2><p class="sub">每项均保留三次实测，不删除慢结果；四项指标使用同一个参数编号。</p></div>{''.join(raw_tables)}<p class="table-note">吞吐与延时分别运行三次，运行模式和随机种子按实际记录列出。p999 每次来自约 10,000 个分组样本，尾部约 10 个，容易受调度波动影响。</p></section>
<section id="parameters"><div class="section-head"><h2>全部初始化参数</h2><p class="sub">按队列只列一次初始化参数，所有消息长度共用。容量条数等随长度变化的值用取值规则列出，完整运行参数保存在导出的 JSON 与原始记录中。</p></div><div class="params-actions actions"><button id="expand-params">展开全部参数</button><button id="collapse-params">折叠全部参数</button></div>{''.join(parameter_groups)}</section>
<section id="method"><div class="section-head"><h2>测试方法与环境</h2><p class="sub">沿用现有结果、负载、复制方式和参数选择规则。</p></div><div class="method-grid">
<div class="method-box"><h3>固定的测试流程</h3><p>每次 1,000 万条，预热 5 万条；业务回调循环 0；完整负载在写入计时前准备，每条的全部数据字均变化。</p><p>每轮上限固定 <strong>214,748,364 B</strong>，即第二层 256 MiB 的 80%；所有队列采用相同边界，末轮按实际条数结束。</p><p>只累计生产者写入时间。轮末交接、尾块发布与回调排空在计时外执行；全部消息仍须消费完成。TwoBufQueue 复制线程与回调线程独立运行。</p><p>{mode_note}</p></div>
<div class="method-box"><h3>综合目标的平台规则</h3><p>使用四种长度全部实测过的共同配置。Writer 取三个按 2 倍递增的相邻块大小；原生 SPSC 取三个相邻自旋或等待值。</p><p>三个配置的四长度综合总分差异不超过 10%；按重复编号合并的四长度综合分变异系数不超过 20%。每个长度的单项原始结果、波动和邻域变化均保存。</p><p>每个配置完整校验通过；三次吞吐与三次延时均成功，零写入失败、零 Writer 背压，延时可解析。参考值的单项变异系数上限仍为 20% / 25% / 40% / 60%。</p><p>所有指标取同一套配置；不合并遍历阶段与旧独立复测阶段的结果。这是所测共同参数范围内的最佳综合平台，不能证明全局最优。</p></div>
<div class="method-box"><h3>本机与编译</h3><p>{esc(env['platform'])} · {esc(env['machine'])} · {env['logical_cpus']} 个逻辑 CPU。</p><p>{esc(env['build']['compiler'].strip())} · Release · -O3 -DNDEBUG · 无 sanitizer。</p><p>生产者 user_interactive QoS，未硬绑核；复制和回调线程不绑核。计时器：mach_absolute_time；每组 1,000 次写入。</p><p>补测使用原测量二进制，SHA-256 一致；开源头文件、完整负载与复制策略不变。补测前已确认接通电源。</p></div>
<div class="method-box"><h3>数据与可复核性</h3><p>原批共 {audit['validation_runs']:,} 次完整校验、{audit['measurement_runs']:,} 次性能测量；失败运行 {audit['failed_runs']}。新增 ReaderWriterQueue 邻域：{extra_audit['validation_runs']} 次完整校验、{extra_audit['measurement_runs']} 次性能测量；失败 {extra_audit['failed_runs']}。</p><p>三份报告共享同一组原生 SPSC 对照。复制策略按表内标注固定；它与编译器生成代码也会影响结果。</p><p>旧逐项选优报告已备份。本页全部四项来自共同参数，参考值固定在补测前；详细运行配置包含 mode、random_seed 的实际值。</p></div></div>
<div class="provenance"><p>测量二进制 SHA-256：<span class="mono">{esc(env['executable_sha256'])}</span></p><div class="files">{''.join(f'<a href="{prefix}{f}">{label}</a>' for f,label in files)}</div></div></section>
<div class="footer">报告更新日期：2026-10-12 · 按指定权重选共同参数 · 256 MiB / 回调循环 0 / 三次平均</div></main>
<script type="application/json" id="report-data">{embedded_json}</script><script>{SCRIPT}</script></body></html>'''
        target = output_dir / FILENAMES[mode]
        target.write_text(content, encoding='utf-8')
        generated.append(target.name)
    (output_dir / 'restyle_manifest.json').write_text(json.dumps({'files':generated,'measurement_sha256':source_hashes,
                                                                 'weighted_selection_sha256':hashlib.sha256(args.selection.read_bytes()).hexdigest(),
                                                                 'weights':selection['policy']['weights'],
                                                                 'configuration_policy':'one per transport/mode, common across four lengths and all metrics'},indent=2)+'\n')
    print(json.dumps({'reports':generated,'measurement_sha256':source_hashes},ensure_ascii=False,indent=2))

if __name__ == '__main__':
    main()
