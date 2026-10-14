"""Offline HTML performance report: fixed-size color groups and a highlighted TwoBufQueue row."""
from pathlib import Path
import html,json,os,statistics

NAMES={'two_buf_queue':'TwoBufQueue','readerwriterqueue':'ReaderWriterQueue','rigtorp':'rigtorp/SPSCQueue','atomic_queue':'atomic_queue'}

def esc(value):return html.escape(str(value),quote=True)
def num(value,places=2):return f'{value:,.{places}f}'

def copy_description(config):
    if config['transport']=='two_buf_queue':
        return 'WriteSession · 固定长度写' if config.get('use_write_session') else '普通 write · 固定长度写'
    if config['transport']=='readerwriterqueue' and config.get('readerwriterqueue_copy_after_zero'):
        return '先清零，再 memcpy'
    if config['transport']=='rigtorp' and config.get('rigtorp_write_prefetch'):
        return '成员复制＋目标写预取'
    return '成员初始化复制'


CSS=r'''
:root{--ink:#163047;--muted:#607386;--line:#d9e3ed;--page:#f3f6fa;--navy:#112b40;--green:#13624d}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:var(--page);color:var(--ink);font:15px/1.65 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
a{color:#1767a7;text-decoration:none}a:hover{text-decoration:underline}button{font:inherit;cursor:pointer}.wrap{max-width:1240px;margin:auto;padding:0 28px}header{background:var(--navy);color:#fff;padding:44px 0 35px}.eyebrow{color:#a3c4dc;letter-spacing:.14em;font-size:12px;font-weight:700}h1{font-size:34px;line-height:1.3;margin:10px 0 14px;letter-spacing:-.02em}header p{color:#c7d9e8;margin:8px 0;max-width:940px}.chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:20px}.chip{background:#244256;border:1px solid #3c586e;border-radius:7px;padding:5px 11px;font-size:12px}.nav{position:sticky;top:0;background:#ffffffed;backdrop-filter:blur(8px);z-index:3;border-bottom:1px solid var(--line)}.nav .wrap{display:flex;align-items:center;justify-content:space-between;gap:20px;padding-top:11px;padding-bottom:11px}.nav-links{display:flex;flex:1;min-width:0;overflow-x:auto;white-space:nowrap;gap:20px;font-size:13px}.nav-links a{flex:0 0 auto}.nav a{color:var(--ink)}.switch{font-size:12px;color:var(--muted);white-space:nowrap}.switch a{color:#1767a7}main{padding-bottom:42px}section{scroll-margin-top:74px;margin:28px 0;background:#fff;border:1px solid var(--line);border-radius:14px;overflow:hidden;box-shadow:0 5px 18px #183c5805}.section-head{padding:22px 24px 14px}h2{font-size:21px;margin:0 0 6px;line-height:1.4}h3{font-size:15px;margin:0 0 10px}.sub{color:var(--muted);font-size:13px;margin:0}.status{display:flex;flex-wrap:wrap;gap:8px;margin:14px 0}.tag{font-size:12px;color:var(--green);background:#eaf6f0;padding:3px 9px;border:1px solid #cfe8dc;border-radius:6px}.tag.warn{color:#885b17;background:#fff6e6;border-color:#f2e2bd}.intro{padding:0 24px 20px}.intro p{margin:8px 0;font-size:13px}.metrics-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px;padding:0 24px 24px}.mini-card{border:1px solid var(--group-line);background:var(--group-bg);border-radius:10px;padding:17px 19px}.badge{font-size:12px;background:var(--group-accent);color:white;padding:3px 9px;border-radius:5px;font-weight:750;white-space:nowrap}.card-meta{float:right;color:var(--muted);font-size:12px}.bar-row{display:grid;grid-template-columns:156px 1fr 84px;gap:12px;align-items:center;font-size:12px;margin-top:12px}.bar-track{height:8px;background:#ffffffb0;border-radius:5px;overflow:hidden}.bar{height:8px;background:#9cb0c2;border-radius:5px}.bar-row.is-tbq{font-weight:750}.bar-row.is-tbq .bar{background:var(--group-accent)}.bar-value{text-align:right;font-variant-numeric:tabular-nums}.filter{display:flex;gap:8px;align-items:center;flex-wrap:wrap;padding:0 24px 18px;font-size:12px;color:var(--muted)}.filter button{border:1px solid var(--line);background:white;color:var(--ink);padding:5px 13px;border-radius:7px;font-size:12px}.filter button[aria-pressed=true]{background:var(--navy);border-color:var(--navy);color:white}.table-scroll{overflow:auto}table{width:100%;border-collapse:separate;border-spacing:0;font-size:13px;white-space:nowrap}th,td{padding:12px 13px;text-align:right;border-bottom:1px solid #ccd9e466;font-variant-numeric:tabular-nums}thead th{background:#eef3f8;color:#4a6278;font-size:11px;font-weight:700;line-height:1.45;border-bottom:1px solid #c6d4e1}th:first-child,td:first-child{text-align:left;padding-left:24px}th:last-child,td:last-child{padding-right:24px}tbody.group td{background:var(--group-bg)}.group-head th{background:var(--group-bg);padding-top:20px;padding-bottom:11px;border-top:1px solid var(--group-line);text-align:left;font-weight:500}.group-head .group-meta{margin-left:13px;color:var(--muted);font-size:12px}.tbq td{font-weight:750;color:#122c43;border-top:2px solid var(--group-accent);border-bottom:2px solid var(--group-accent)}.tbq td:first-child{border-left:5px solid var(--group-accent);padding-left:19px}.tbq td:last-child{border-right:2px solid var(--group-accent);padding-right:22px}.range{display:block;font-size:10px;color:var(--muted);font-weight:400;margin-top:3px}.table-note{padding:13px 24px 19px;color:var(--muted);font-size:12px;line-height:1.75;margin:0}.unit{font-weight:400;color:#72879a}.group-8{--group-bg:#eaf3fc;--group-line:#b9d4ee;--group-accent:#246ca7}.group-16{--group-bg:#f1ecfa;--group-line:#d0c1ec;--group-accent:#7653a4}.group-32{--group-bg:#e9f5ee;--group-line:#b9ddc9;--group-accent:#2b7c5d}.group-64{--group-bg:#fff3df;--group-line:#ead1a4;--group-accent:#a7681d}[hidden]{display:none!important}.run-set{margin:0 24px 14px;border:1px solid var(--group-line);border-radius:9px;overflow:hidden;background:var(--group-bg)}summary{cursor:pointer;padding:13px 16px;font-size:13px;font-weight:650}summary .badge{margin-right:10px}.run-set table{font-size:12px}.run-set th:first-child,.run-set td:first-child{padding-left:16px}.run-set td{background:var(--group-bg)}.method-grid{display:grid;grid-template-columns:1fr 1fr;gap:18px;padding:0 24px 22px}.method-box{border:1px solid var(--line);border-radius:9px;padding:16px 18px;font-size:13px}.method-box p{margin:7px 0}.method-box strong{font-weight:700}.callout{margin:0 24px 20px;background:#fff7e9;border-left:4px solid #c9903f;padding:13px 16px;font-size:12px;color:#75531f;border-radius:4px}.params{margin:0 24px 20px;border:1px solid var(--line);border-radius:9px}.params summary{background:#f6f9fc}.params pre{margin:0;padding:17px;overflow:auto;max-height:440px;white-space:pre-wrap;word-break:break-word;background:#f8fafc;font:11px/1.7 ui-monospace,SFMono-Regular,Menlo,monospace}.provenance{padding:0 24px 24px;font-size:12px;color:var(--muted)}.files{display:flex;flex-wrap:wrap;gap:8px;margin-top:11px}.files a,.actions button{padding:5px 10px;border:1px solid var(--line);background:#f8fafc;border-radius:6px;font-size:12px}.actions{display:flex;gap:7px}.footer{font-size:12px;color:var(--muted);padding-top:10px}.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;word-break:break-all;font-size:11px}body:has(button[aria-pressed=true][data-filter]:not([data-filter=all])) .metrics-grid{grid-template-columns:minmax(0,1fr)}
@media(max-width:850px){.wrap{padding:0 16px}h1{font-size:27px}.metrics-grid,.method-grid{grid-template-columns:1fr}.nav-links{gap:12px}.switch{display:none}.bar-row{grid-template-columns:142px 1fr 78px;gap:8px}.section-head{padding-left:18px;padding-right:18px}.table-scroll table{min-width:920px}.filter{padding-left:18px}.nav .wrap{gap:10px}.metrics-grid{padding-left:16px;padding-right:16px}.mini-card{padding:14px}.actions button{padding:5px 7px}.intro,.provenance{padding-left:18px;padding-right:18px}}
@media print{body{background:white;font-size:12px}header{padding:20px 0;background:#112b40!important;-webkit-print-color-adjust:exact;print-color-adjust:exact}.wrap{max-width:none;padding:0 14px}.nav,.filter,.actions,.switch{display:none}section{box-shadow:none;break-inside:auto;margin:16px 0;border-radius:5px}.mini-card,tr,.method-box{break-inside:avoid}table{font-size:10px}th,td{padding:8px 7px}.table-scroll{overflow:visible}.group td,.mini-card{print-color-adjust:exact;-webkit-print-color-adjust:exact}h1{font-size:25px}.run-set:not([open]){display:none}.metrics-grid{gap:10px}.section-head{padding:15px 16px 10px}}
'''

def write_html_report(data,path,output):
    path=Path(path);output=Path(output);path.parent.mkdir(parents=True,exist_ok=True)
    tuned=bool(data.get('search'))
    profile=data['profile'];cache=profile['cache_mib'];policy=profile['callback_policy'];groups=data['groups']
    lengths=profile['payload_bytes'];reps=profile['repetitions'];messages=profile['messages']
    title=f'{cache} MiB 缓存 · '+('回调时间校准' if policy=='calibrated' else '回调循环固定' if policy=='fixed' else '回调循环为 0')
    write_mode=profile.get('write_mode')
    if write_mode: title+=' · '+('单块 Session 双重循环' if write_mode=='session' else '普通 write')
    batch_size=groups[0]['parameters']['latency_batch_size']
    cpu=groups[0]['parameters']['producer_cpu']
    qos=groups[0]['parameters']['producer_qos']
    scheduling=('高 QoS' if qos=='user_interactive' else '默认 QoS')+' · '+('未硬绑核' if cpu<0 else f'CPU {cpu} 绑核')
    timer_labels=' / '.join(groups[0]['timer'])

    peer='performance_8m_zero.html' if cache==256 else 'performance_256m_callback.html'
    peer_label='8 MiB · 回调 0' if cache==256 else '256 MiB · 回调工作'
    if write_mode=='session': peer=peer.replace('.html','_session.html')
    switch_links=f'<a href="{peer}">{peer_label}</a>'
    if profile.get('report_peers'):
        switch_links=' · '.join(f'<a href="{esc(p["file"])}">{esc(p["label"])}</a>' for p in profile['report_peers'] if p['file']!=path.name)
    relative=os.path.relpath(output,path.parent).replace(os.sep,'/')
    cards=[];performance=[];pressure=[];runs=[]
    for length in lengths:
        gg=[g for g in groups if g['payload_bytes']==length]
        iterations=gg[0]['callback_iterations'];rounds=gg[0]['completed_rounds'][0]
        cal=next((c for c in data['calibration'] if c['payload_bytes']==length),None)
        label=f'回调循环 {iterations} 次 · 每次 {rounds} 轮 · {messages:,} 条'
        card=[f'<article class="mini-card group-{length}" data-payload="{length}"><span class="badge">{length} B</span><span class="card-meta">吞吐 · M条/s</span>']
        maximum=max(g['throughput_mps'] for g in gg)
        for g in gg:
            card.append(f'<div class="bar-row {"is-tbq" if g["transport"]=="two_buf_queue" else ""}"><span>{NAMES[g["transport"]]}</span><div class="bar-track"><div class="bar" style="width:{g["throughput_mps"]/maximum*100:.2f}%"></div></div><span class="bar-value">{num(g["throughput_mps"])}</span></div>')
        card.append('</article>');cards.append(''.join(card))
        head=f'<tr class="group-head"><th colspan="9"><span class="badge">{length} B</span><span class="group-meta">{esc(label)}</span></th></tr>'
        performance.append(f'<tbody class="group group-{length}" data-payload="{length}">{head}')
        pressure.append(f'<tbody class="group group-{length}" data-payload="{length}"><tr class="group-head"><th colspan="9"><span class="badge">{length} B</span><span class="group-meta">正式吞吐运行的逐次平均；RSS 为本组最大值</span></th></tr>')
        run=[f'<details class="run-set group-{length}" data-payload="{length}"><summary><span class="badge">{length} B</span>{reps} 次原始值 · 回调循环 {iterations} 次</summary><div class="table-scroll"><table><thead><tr><th>实现 / 指标</th>'+''.join(f'<th>第 {i+1} 次</th>' for i in range(reps))+'<th>平均</th></tr></thead><tbody>']
        for g in gg:
            special=' class="tbq"' if g['transport']=='two_buf_queue' else ''
            values=g['throughput_runs_mps']
            performance.append(f'<tr{special}><td>{NAMES[g["transport"]]}<span class="range">{esc(copy_description(g["parameters"]))}</span></td><td>{num(g["throughput_mps"])}<span class="range">{num(min(values))} – {num(max(values))}</span></td><td>{num(g["payload_gb_per_second"],3)}</td><td>{num(g["p50_ns"],3)}</td><td>{num(g["p99_ns"],3)}</td><td>{num(g["p999_ns"],3)}</td><td>{num(g["callback_to_producer_ratio"])}×</td><td>{iterations}</td><td>{rounds}</td></tr>')
            internal=g['internal_backpressure']
            def pvalue(key):return num(internal[key],0) if internal is not None else '—'
            pressure.append(f'<tr{special}><td>{NAMES[g["transport"]]}</td><td>{num(g["producer_seconds"]*1000,2)}</td><td>{num(g["callback_processing_seconds"]*1000,2)}</td><td>{num(g["backpressured_messages"],1)}</td><td>{num(g["failed_write_calls"],1)}</td><td>{num(g["producer_retry_seconds"]*1000,3)}</td><td>{pvalue("writer_pressure_events")}</td><td>{pvalue("copy_memory_full_events")}</td><td>{num(g["peak_rss_mib"],1)}</td></tr>')
            measures=[('吞吐 M条/s',values,g['throughput_mps'])]+[(q+' ns',[r[q] for r in g['latency_runs']],g[q+'_ns']) for q in ('p50','p99','p999')]
            for metric,vals,average in measures:
                run.append(f'<tr{special}><td>{NAMES[g["transport"]]}<span class="range">{metric}</span></td>'+''.join(f'<td>{num(v,3)}</td>' for v in vals)+f'<td>{num(average,3)}</td></tr>')
        performance.append('</tbody>');pressure.append('</tbody>');run.append('</tbody></table></div></details>');runs.append(''.join(run))
    copy_strategy=data['environment'].get('build',{}).get('payload_copy_strategy','member_copy')
    copy_note={'member_copy':'成员初始化 bytes(other.bytes)：一次完整复制',
               'member_copy_optional_rigtorp_destination_prefetch':'一次成员复制；rigtorp 可选目标写预取',
               'member_copy_rigtorp_prefetch_readerwriterqueue_optional_zero':'完整负载复制；rigtorp 可选目标写预取，ReaderWriterQueue 可选先清零再 memcpy'}.get(copy_strategy,copy_strategy)
    sensitivity=data.get('copy_sensitivity')
    sensitivity_html=''
    if sensitivity:
        control_rows=[]
        control_prefix=esc(sensitivity.get("source_prefix", "performance_data/"+sensitivity["data_directory"]))
        for control in sensitivity['groups']:
            transport,length=control['label'].rsplit('_',1)
            control_rows.append(f'<tbody class="group group-{length}"><tr><td>{NAMES[transport]} · {length} B</td><td>{num(control["member_init"]["p50_ns"],3)}</td><td>{num(control["body_memcpy"]["p50_ns"],3)}</td></tr></tbody>')
        sensitivity_html=f'<section id="copy-sensitivity"><div class="section-head"><h2>先看负载复制的敏感性</h2><p class="sub">8 MiB、空回调的独立 A/B：每次 1,000 万条，5 次平均；只改外部 Payload 复制构造，完整校验通过。</p><p class="sub">这是此前两种复制方式的独立实验，未使用本次新增的写预取；不计入本轮主表运行次数；记录时间：{esc(sensitivity.get("timestamp", "未记录"))}。</p></div><div class="callout"><strong>本次正式表格固定采用 {esc(copy_note)}。</strong>仅改变复制构造就能显著改变部分开源队列的结果。后面的差距包含负载类型与编译器代码生成的影响，不能将它全部算成队列算法的优势。具体处理器层面的原因尚未确定；默认复制构造的额外校验也未消除异常。</div><div class="table-scroll"><table><thead><tr><th>实现 / 长度</th><th>成员初始化 · p50 ns</th><th>构造函数体 memcpy · p50 ns</th></tr></thead>{"".join(control_rows)}</table></div><p class="table-note">两个版本的默认成员都有 bytes{{}}；构造函数体 memcpy 版本会先执行默认初始化。全部试验保留，没有按队列分别选择复制方式。<a href="{control_prefix}/summary.json">对照汇总</a> · <a href="{control_prefix}/raw.jsonl">144 次原始记录</a> · <a href="{control_prefix}/variant-source.tar.gz">对照源码</a></p></section>'
    calibrations=''
    if data['calibration']:
        calibrations='<p><strong>逐长度校准结果：</strong>'+ '；'.join(f'{c["payload_bytes"]} B：{c["selected_iterations"]} 次循环，校准时间比 {c["selected_ratio"]:.2f}×'+('（目标未达到）' if not c['target_met'] else '') for c in data['calibration'])+'。</p>'
    min_samples=min(min(g['latency_samples']) for g in groups);max_samples=max(max(g['latency_samples']) for g in groups)
    failed=sum(g['failed_write_calls'] for g in groups)
    warnings=[]
    for g in groups:
        if max(g['throughput_runs_mps'])/min(g['throughput_runs_mps'])>1.25:
            warnings.append(f'{g["payload_bytes"]} B · {NAMES[g["transport"]]}：{num(min(g["throughput_runs_mps"]))}–{num(max(g["throughput_runs_mps"]))} M条/s')
    variability='<p>波动较大的吞吐组：'+esc('；'.join(warnings))+'。所有测量均保留，未删除低值。</p>' if warnings else '<p>所有逐次测量值都已保留，未删除低值。</p>'
    measured_tbq=[g for g in groups if g['transport']=='two_buf_queue']
    actual_buffers='TwoBufQueue 的第一层与第二层各 '+str(cache)+' MiB，合计 '+str(cache*2)+' MiB；三种开源 SPSC 请求 '+str(cache)+' MiB。ReaderWriterQueue 会按内部块容量略微取整。'
    params={f'{g["payload_bytes"]} B / {g["transport"]}':g.get('parameters_by_objective',g['parameters']) for g in groups}
    embedded=json.dumps(data,ensure_ascii=False).replace('<','\\u003c')
    file_links=['reproduction.json'] if (output/'reproduction.json').exists() else []
    file_links += ['results.json','raw.jsonl','profile.json','environment.json','audit.json','dependencies.json']
    if data['calibration']:file_links.append('callback-calibration.json')
    if tuned:file_links+=['best.json','candidates.csv','search-raw.jsonl']
    search_note=('在声明的参数空间与预算内搜索；吞吐、p50、p99、p999 分别选优，并各做独立正式复测。表中不同指标可能来自不同配置，完整参数按指标列出。' if tuned else '固定本次参数进行对比；本报告没有执行全参数寻优，也不声称全局最优。')
    block_note=('源块 / 输出块按指标分别选择；每层数据字节预算保持不变。' if tuned else '源块 / 输出块均为 64 KiB。')
    block_note=profile.get('block_note',block_note)
    publish_note=('publish_count 按指标分别选择。' if tuned else f'publish_count 固定为 {profile["publish_count"]}。')
    completed_only=any(not g['parameters'].get('is_scan_current',True) or g['parameters']['publish_count']==2**32-1 for g in measured_tbq)
    flush_note='不逐条 flush · 轮末计时外交接' if completed_only else '扫描当前块 · 不主动 flush'
    round_note=('每轮上限按缓存的 80% / 消息字节数计算。TwoBufQueue 不扫描当前块，publish_count 为 UINT32_MAX；停止发送计时后调用 finish_block 交接当前块，再等待本轮全部回调完成。预热也在计时外交接，最终尾部统一 finish。没有增加额外消息，原生 SPSC 每条已发布，轮末无需额外交接。' if completed_only else '每轮上限按缓存的 80% / 消息字节数计算，再停在实际发布批次的整数倍位置。TwoBufQueue 开启当前块扫描，无逐条或轮末主动 flush。停止写线程计时后等待本轮全部回调完成；最终尾部由正常 finish 收尾。')
    input_note=('吞吐和分组延时均在计时前准备整轮输入；每条消息的全部字都随序号变化；四种实现使用同一完整负载；队列内仍复制一次完整负载；各行标明成员复制、目标写预取或先清零再 memcpy，完整参数记录实际选择；先清零的额外存储也包含在计时中。输入缓冲单独计算，不算队列容量，也不保证常驻 L1。' if groups[0]['parameters'].get('prepared_input') else '吞吐与延时的输入准备位置见原始配置与日志。')
    audit_notes=''.join('<p>'+esc(note)+'</p>' for note in data.get('audit_notes',[]))
    timing_policy='逐长度以 TwoBufQueue 的吞吐测量为基准，以整轮回调区间达到写线程时间的 2～3 倍为校准目标，未达到时保留实际值；该长度的四个实现使用同一循环次数。其他实现写入速度不同，其时间比可以不在 2～3 倍内。' if policy=='calibrated' else '每种长度的回调业务循环次数固定，各实现共享；本轮未重新校准到 2～3 倍，表格保留实际时间比。' if policy=='fixed' else '每条消息的模拟业务循环为 0；仍保留逐消息回调、序号/条数校验和队列自身的消费工作。'
    document=f'''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>{esc(title)} · TwoBufQueue 性能测试</title><style>{CSS}</style></head><body>
<header><div class="wrap"><div class="eyebrow">TWOBUFQUEUE / PERFORMANCE NOTES</div><h1>{esc(title)}</h1><p>固定长度、不写长度前缀的 TwoBufQueue，与三种开源 SPSC 队列对比。TwoBufQueue 包含完整第二层复制与独立回调线程；第三方使用原生消费线程逐条回调。</p><div class="chips"><span class="chip">{messages/1000000:g} 百万条 / 次</span><span class="chip">{reps} 次平均 / 指标</span><span class="chip">{' / '.join(str(n)+' B' for n in lengths)}</span><span class="chip">publish_count = {profile['publish_count']}</span><span class="chip">{esc(flush_note)}</span><span class="chip">{esc(scheduling)}</span></div></div></header>
<div class="nav"><div class="wrap"><div class="nav-links"><a href="#overview">总览</a><a href="#performance">吞吐与延时</a><a href="#pressure">时间与背压</a><a href="#runs">原始值</a><a href="#method">测试口径</a></div><span class="switch">其他报告：{switch_links}</span><div class="actions"><button id="export-json">导出数据</button><button id="print-report">打印</button></div></div></div>
<main class="wrap"><section id="overview"><div class="section-head"><h2>按消息长度查看吞吐</h2><p class="sub">条形长度仅在同一消息长度内比较。单位 M条/s = 每秒百万条消息。</p><div class="status"><span class="tag">{data['formal_runs']} 次正式测量</span><span class="tag">{data['validation_runs']} 次负载校验</span><span class="tag">消息计数 / FIFO 校验通过</span><span class="tag">{'写入失败为 0' if failed==0 else '存在写入重试，详见下表'}</span><span class="tag warn">p999 样本量提示</span></div></div><div class="intro"><p>{esc(actual_buffers)}</p><p>{esc(timing_policy)}</p>{calibrations}</div><div class="filter"><span>消息长度</span><button data-filter="all" aria-pressed="true">全部</button>{''.join(f'<button data-filter="{n}" aria-pressed="false">{n} B</button>' for n in lengths)}</div><div class="metrics-grid">{''.join(cards)}</div></section>
<section id="performance"><div class="section-head"><h2>吞吐与写入耗时</h2><p class="sub">每个指标先按单次运行计算，再取 {reps} 次算术平均。TwoBufQueue 整行加粗并加边框。{esc("各指标使用独立选择的配置。" if tuned else "")}</p></div><div class="table-scroll"><table id="performance-table"><thead><tr><th>实现</th><th>吞吐<br><span class="unit">M条/s · 下方为范围</span></th><th>有效负载<br><span class="unit">GB/s</span></th><th>p50<br><span class="unit">ns / 次</span></th><th>p99<br><span class="unit">ns / 次</span></th><th>p999<br><span class="unit">ns / 次</span></th><th>回调区间 / 写入<br><span class="unit">墙钟时间比</span></th><th>回调循环<br><span class="unit">次 / 消息</span></th><th>分轮<br><span class="unit">轮 / 运行</span></th></tr></thead>{''.join(performance)}</table></div><p class="table-note">p50 / p99 / p999 是“每组 {batch_size} 次成功写入的平均耗时”的分位数，不是单条消息端到端延时，也不是单次 write 的分位数。吞吐使用独立的未逐组计时运行。GB/s 按业务净负载计算。时间比 = 各轮首条回调开始至末条回调结束的耗时之和 ÷ 各轮写线程发送耗时之和，再对运行取平均；它包含分发、等待数据和调度，不是每次回调函数 CPU 耗时之和。</p></section>
<section id="pressure"><div class="section-head"><h2>回调时间与背压</h2><p class="sub">与吞吐表使用同一批正式运行。时间、次数按运行取平均；峰值 RSS 为这组运行的最大值。</p></div><div class="table-scroll"><table><thead><tr><th>实现</th><th>写线程<br><span class="unit">ms</span></th><th>回调区间<br><span class="unit">ms</span></th><th>受压消息<br><span class="unit">条 / 次运行</span></th><th>失败尝试<br><span class="unit">次 / 运行</span></th><th>重试时间<br><span class="unit">ms</span></th><th>Writer 压力<br><span class="unit">内部事件</span></th><th>复制层拒绝<br><span class="unit">内部事件</span></th><th>峰值 RSS<br><span class="unit">MiB</span></th></tr></thead>{''.join(pressure)}</table></div><p class="table-note">受压消息至少经历一次 try 写入失败；失败尝试含重复重试，最终仍全部写入。Writer 与复制层内部事件仅 TwoBufQueue 提供，其他实现显示“—”，不能把“—”解释为 0。回调区间含分发、轮内等待及调度，时间比大于 1 本身不等于写端背压。RSS 包含进程开销，不等于缓存容量。</p></section>
<section id="runs"><div class="section-head"><h2>逐次测量值</h2><p class="sub">展开查看全部 {reps} 次吞吐及分位数。{esc("各指标来自各自最优配置的独立测量。" if tuned else "")}没有剔除异常值，也没有将多个运行的样本合并计算分位数。</p></div>{''.join(runs)}<div class="intro">{variability}</div></section>
<section id="method"><div class="section-head"><h2>测试口径与复现信息</h2><p class="sub">{esc(search_note)}</p></div><div class="method-grid"><div class="method-box"><h3>发送、复制与排空</h3><p>TwoBufQueue：Writer → 复制线程 → 第二层缓存 → 独立回调线程，逐消息调用回调。</p><p>{esc(block_note)}四种实现正式写入次数与净负载长度相同。</p><p>{esc(round_note)}</p><p>两份报告同时改变缓存大小与回调工作量，不能将两组差异全部归因于缓存；比较容量影响需另做相同回调负载的对照。</p><p>{esc(flush_note)}；{esc(publish_note)}仅预触碰写端数据页，第二层数据页没有额外预触碰。</p><p>{esc(input_note)}</p></div><div class="method-box"><h3>时钟、调度与限制</h3><p>本机为 {esc(data['environment']['platform'])} / {esc(data['environment']['machine'])}，Release 优化构建。</p><p>计时器：{esc(timer_labels)}；每组 {batch_size} 次写入。组均值刻度约 {groups[0]['latency_quantum_ns']:.4f} ns / 次。</p><p>写线程调度：{esc(scheduling)}。调度打断和其他系统活动仍可能影响结果；复制与回调线程保留自身调度策略。</p><p>吞吐只累计写线程发送时间；排空在计时之外。并发复制 / 回调仍可能争用缓存和带宽，因此不是持续端到端吞吐。</p></div></div><div class="callout"><strong>p999 仅供观察：</strong>每次延时测量约有 {min_samples:,}–{max_samples:,} 个组均值样本，少于框架建议的 100,000 个。p999 的尾部覆盖有限，不能据此做稳定的尾延时承诺。p999 表示 99.9% 分位数；平均值不代表置信区间。</div><details class="params"><summary>完整构造参数 · 每种长度 / 实现 / 优化指标</summary><pre>{esc(json.dumps(params,ensure_ascii=False,indent=2))}</pre></details><div class="provenance"><div>生成于 {esc(data['environment']['timestamp'])} · 共 {data['formal_runs']} 次正式运行，{data['validation_runs']} 次负载校验，{data['calibration_runs']} 次回调校准。</div><div>编译器：{esc(data['environment']['build']['compiler'])}</div><div>被测程序 SHA-256：<span class="mono">{esc(data['environment']['binary_sha256'])}</span></div><div class="files">{''.join(f'<a href="{esc(relative)}/{name}">{name}</a>' for name in file_links)}</div><p>第三方原文件按 benchmarks/spsc/spsc_dependencies.json 校验，未修改队列实现；外部 Payload 的复制策略为 {esc(copy_note)}，复制完整负载。逐次命令、配置、校验结果与内部计数保存在 raw.jsonl。</p></div></section>
<div class="footer">此 HTML 内嵌汇总数据，离线可查看、筛选与导出。原始日志链接需要同目录下的 performance_data 文件夹。</div></main>
<script type="application/json" id="report-data">{embedded}</script><script>
const filterButtons=[...document.querySelectorAll('[data-filter]')];filterButtons.forEach(button=>button.addEventListener('click',()=>{{const chosen=button.dataset.filter;filterButtons.forEach(b=>b.setAttribute('aria-pressed',String(b===button)));document.querySelectorAll('[data-payload]').forEach(el=>el.hidden=chosen!=='all'&&el.dataset.payload!==chosen)}}));document.querySelector('#print-report').addEventListener('click',()=>window.print());document.querySelector('#export-json').addEventListener('click',()=>{{const blob=new Blob([document.querySelector('#report-data').textContent],{{type:'application/json'}});const url=URL.createObjectURL(blob);const a=document.createElement('a');a.href=url;a.download='spsc-performance-{cache}m.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000)}});
</script></body></html>'''
    document=document.replace(calibrations+'</div><div class="filter">',calibrations+audit_notes+'</div><div class="filter">',1)
    document=document.replace('<section id="overview">',sensitivity_html+'<section id="overview">')
    path.write_text(document,encoding='utf-8')
    return path
