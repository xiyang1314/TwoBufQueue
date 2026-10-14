"""One-table presentation of all prespecified 1 MiB tail conditions."""
import json,html,os
from pathlib import Path
NAMES={'TwoBufQueue_yield1024':'TwoBufQueue · 每1,024次重试yield',
       'TwoBufQueue_yield0':'TwoBufQueue · 重试不yield',
       'TwoBufQueue_yield65536':'TwoBufQueue · 每65,536次重试yield',
       'ReaderWriterQueue':'ReaderWriterQueue','rigtorp':'rigtorp::SPSCQueue','atomic_queue':'atomic_queue'}

def render(folder,report,result):
    prefix=os.path.relpath(folder,report.parent).replace(os.sep,'/')
    profile_reference = os.path.relpath(folder/'selected_config.json', Path(__file__).resolve().parents[2]).replace(os.sep, '/')
    profile=result['parameters'];rows=result['rows'];base=rows[0]
    stable=[]
    for row in rows[1:3]:
        if all(row['per_run'][i][k]<base['per_run'][i][k] for i in range(3) for k in ('p99','p999')):stable.append(row['queue'])
    verdict=('三轮中p99、p999均优于原重试策略：'+', '.join(NAMES[x] for x in stable)) if stable else '本次未验证到三轮p99、p999均稳定改善的策略。保留全部结果，继续使用原参数。'
    h='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>1 MiB发送后的业务尾部耗时</title><style>body{margin:0;background:#f3f6fb;color:#183048;font:16px/1.7 -apple-system,sans-serif}main{max-width:1050px;margin:auto;padding:25px}section{background:white;padding:24px;border-radius:15px;margin:18px 0;border:1px solid #dce5ee}h1{font-size:30px}table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}th,td{padding:12px;text-align:right;border-bottom:1px solid #dce5ee}th:first-child{text-align:left}thead{background:#edf2f8}.tb>*{background:#dff4ee;color:#075c4e;font-weight:700}.p50{background:#183d55;color:white;font-weight:750}.tb>.p50{background:#075c4e;color:white}.scroll{overflow:auto}pre{white-space:pre-wrap;background:#f3f6fa;padding:15px}a{color:#087f6e}summary{cursor:pointer;font-weight:700}</style><main><section><h1>发送1 MiB，业务尾部耗时能否降低？</h1>'''
    h+=f'<p>{html.escape(verdict)}</p><p>固定{profile["selected_business_kib"]} KiB业务工作集、32 B消息、每条回调{profile["callback_iterations"]}次循环。TwoBufQueue第一层16 KiB（4×4 KiB），第二层及开源槽位存储256 MiB。扫描关闭、publish为最大值，回调持续运行，目标积压80%。写线程高QoS，后台维持原调度，未硬绑核。</p></section>'
    h+='<section><h2>性能结果</h2><p>每轮发送2,500批，每批1 MiB（32,768条）。发送后立即执行64次业务，每16次计时一组，共10,000组/轮。每轮独立统计分位数，再对三轮取平均；所有数值越小越好。</p><div class="scroll"><table><thead><tr><th>queue / 重试策略</th><th class="p50">p50</th><th>p99</th><th>p999</th><th>前耗时</th></tr></thead><tbody>'
    for row in rows:
        h+=f'<tr class="{"tb" if row["queue"].startswith("TwoBufQueue") else ""}"><th>{NAMES[row["queue"]]}</th><td class="p50">{row["p50"]:.1f}</td>'
        h+=''.join(f'<td>{row[k]:.1f}</td>' for k in ('p99','p999','before_ns'))+'</tr>'
    h+='</tbody></table></div><p>单位：ns/次。后耗时为每16次业务的组均值分位数。重试策略只在写入暂时失败时生效；等待发生在发送阶段，未直接计入后业务时间。</p></section>'
    h+='<section><details><summary>三轮原值、写入时间与验证</summary><p>改动仅使retry_yield_every参数在突发测试中实际生效：0关闭主动yield；成功写入、消息内容、回调及计时窗口均保持一致。没有过滤慢样本。</p><pre>'+html.escape(json.dumps(rows,ensure_ascii=False,indent=2))+'</pre></details>'
    h+=f'<details><summary>复现与参数明细</summary><pre>python3 benchmarks/burst/run_burst_tail_1m.py --profile {profile_reference}</pre><p>一个命令完成构建、六个条件、三轮完整采样及报告；不需要额外采样。</p><p><a href="{prefix}/report_results.json">全部统计</a> · <a href="{prefix}/manifest.json">运行清单</a> · <a href="{prefix}/selected_config.json">初始化参数</a></p><pre>'+html.escape(json.dumps(profile,ensure_ascii=False,indent=2))+'</pre></details></section></main></html>'
    report.parent.mkdir(parents=True,exist_ok=True);report.write_text(h)
