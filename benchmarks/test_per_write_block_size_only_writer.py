"""32 B single-thread Writer/pool block-size sweep, both publication strategies."""
from pathlib import Path
import argparse
import csv
import datetime
import html
import json
import platform
import random
import statistics
import subprocess
import sys
import time

from test_per_writer_block_size import save, sha, probe, memory_state, pool_summary, size_label, number, round_bytes_for

ROOT = Path(__file__).resolve().parents[1]
GIB = 1024**3
MAX_PUBLISH = 4294967295


def summarize(rows, repetitions):
    summary = []
    for publish, size in sorted({(r['publish_count'], r['writer_block_bytes']) for r in rows}):
        measured = [r for r in rows if r['publish_count'] == publish and r['writer_block_bytes'] == size
                    and r['stage'] == 'measurement']
        throughput = [r['result'] for r in measured if r['mode'] == 'throughput']
        latency = [r['result'] for r in measured if r['mode'] == 'write_latency']
        if len(throughput) != repetitions or len(latency) != repetitions:
            continue
        value = dict(publish_count=publish, writer_block_bytes=size, parameters=throughput[0]['config'],
                     throughput_mps=statistics.fmean(r['m_messages_per_second'] for r in throughput),
                     throughput_runs_mps=[r['m_messages_per_second'] for r in throughput],
                     payload_gb_per_second=statistics.fmean(r['payload_gb_per_second'] for r in throughput),
                     peak_rss_mib=max(r['peak_rss_mib'] for r in throughput+latency),
                     writer_pool=pool_summary(throughput, 'writer'),
                     latency_writer_pool=pool_summary(latency, 'writer'))
        for key in ('producer_seconds', 'input_preparation_seconds', 'round_completion_seconds', 'wall_seconds',
                    'cpu_seconds', 'retired_messages', 'completed_blocks', 'natural_switches', 'rounds',
                    'notification_calls', 'empty_notifications', 'failed_write_calls', 'writer_pressure_events'):
            value[key] = statistics.fmean(r[key] for r in throughput)
        for key in ('p50', 'p99', 'p999'):
            value[key+'_ns'] = statistics.fmean(r['batch_average_write_ns'][key] for r in latency)
            value[key+'_runs_ns'] = [r['batch_average_write_ns'][key] for r in latency]
        value['latency_samples'] = [r['latency_samples'] for r in latency]
        summary.append(value)
    return summary


def write_report(path, rows, environment, publish, peer):
    fastest = max(rows, key=lambda r: r['throughput_mps'])
    lowest = min(rows, key=lambda r: r['p50_ns'])
    performance, depth = [], []
    for row in rows:
        label = html.escape(size_label(row['writer_block_bytes']))
        rr = row['throughput_runs_mps']
        best = ' class="best"' if row is fastest else ''
        performance.append(f'<tr{best}><th>{label}</th><td>{number(row["throughput_mps"])}</td>'
                           f'<td>{number(row["payload_gb_per_second"])}</td><td>{number(row["p50_ns"])}</td>'
                           f'<td>{number(row["p99_ns"])}</td><td>{number(row["p999_ns"])}</td>'
                           f'<td>{number(min(rr))}–{number(max(rr))}</td><td>{number(row["peak_rss_mib"], 1)}</td></tr>')
        w = row['writer_pool']
        samples_per_run = w['samples']/len(w['runs'])
        depth.append(f'<tr><th>{label}</th><td>{number(samples_per_run,0)}</td>'
                     f'<td>{number(row["natural_switches"],0)}</td><td>{number(row["completed_blocks"],0)}</td>'
                     f'<td>{w["max_in_use"]}</td><td>{number(w["average_in_use"])}</td>'
                     f'<td>{w["max_allocated"]}</td><td>{number(w["average_allocated"])}</td>'
                     f'<td>{number(w["max_allocated_bytes"]/1024**2,2)}</td>'
                     f'<td>{number(row["writer_pressure_events"],0)}</td></tr>')
    settings = environment['settings']
    strategy = '每条 release 发布 · publish_count = 1' if publish == 1 else '仅交接发布 · publish_count = UINT32_MAX'
    scheduling = ('高 QoS，未硬绑核' if settings['producer_qos'] == 'user_interactive'
                  else '平台默认 QoS，未硬绑核') if settings['producer_cpu'] < 0 else f'绑定 CPU {settings["producer_cpu"]}'
    escaped = html.escape(json.dumps(environment, ensure_ascii=False, indent=2))
    page = f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TwoBufQueue · 单线程 Writer · {publish}</title>
<style>body{{margin:0;background:#f4f7fb;color:#17243b;font:15px/1.65 system-ui,-apple-system,sans-serif}}main{{max-width:1450px;margin:auto;padding:32px 24px}}h1{{font-size:29px;margin:0 0 12px}}h2{{margin:28px 0 10px;font-size:20px}}.card{{background:white;padding:22px;border:1px solid #dce5ef;border-radius:12px;margin-bottom:18px}}.scroll{{overflow:auto;border:1px solid #dce5ef;border-radius:10px}}table{{border-collapse:collapse;width:100%;white-space:nowrap;font-variant-numeric:tabular-nums}}td,th{{padding:11px 13px;text-align:right;border-bottom:1px solid #dce5ef}}thead th{{background:#213b60;color:white;font-size:13px}}tbody th{{text-align:left}}tbody tr{{background:#edf5ff}}tbody tr:nth-child(even){{background:#e5effc}}tbody tr:hover{{background:#d5e7fb}}tr.best{{background:#d5f0e7!important;font-weight:650}}.muted{{color:#53667f}}.tag{{display:inline-block;background:#e5effc;padding:4px 10px;margin:0 7px 8px 0;border-radius:6px}}pre{{white-space:pre-wrap;font-size:12px;overflow:auto}}a{{color:#1766ab}}.bestnote{{color:#076644}}</style>
<main><h1>单线程 Writer：块大小与发布策略</h1><div class="card">
<span class="tag">32 B 普通 write</span><span class="tag">{strategy}</span><span class="tag">{settings['repetitions']} 次平均</span><span class="tag">{scheduling}</span>
<p>每项 {settings['messages']:,} 条，预热 {settings['warmup_messages']:,} 条。writer 池容量上限 {size_label(settings['writer_cache_bytes'])}，初始 {settings['initial_pool_blocks']} 块。
只实例化真实 Writer、WriterPool 和 Writer 所需的交接队列；没有读线程、第二层池、第二层复制或业务回调。</p>
<p class="muted">沿用现有普通 write 的预生成输入和逐条发送循环：32 字节全部随序号变化，数据准备、初始预触碰和预热不计入写入时间。
块满后，Writer 按原代码取得新块并交接旧块；现有通知在当前写线程内直接确认完成记录并归还旧块。自然换块、发布、取块、归还和深度统计均计时；不在每条消息后额外检查换块。</p>
<p class="muted">两种策略都沿用 reader 实际总容量 80% 的分轮上限；本测试仅使用同配置 reader 容量作为计算参考，不创建 reader 池。
显式轮容量覆盖值为 {size_label(settings['round_storage_bytes'])}（0 B 表示自动计算）。预热后都交接尾块，从无当前块状态开始正式测量。
每轮计时结束后 finish_block 并立即归还尾块，尾部交接不计时，下一轮申请块仍计时。正式运行不读取消息负载；每个块大小、每种策略另有独立进程校验全部字节和 FIFO 顺序。
吞吐是单线程写入与即时归还的成绩，不是完整 TwoBufQueue 流水线吞吐。</p>
<p class="bestnote">吞吐最高：{size_label(fastest['writer_block_bytes'])}，{number(fastest['throughput_mps'])} M条/s；p50 最低：{size_label(lowest['writer_block_bytes'])}，{number(lowest['p50_ns'])} ns。
<a href="{html.escape(peer.name)}">查看另一种发布策略</a></p></div>
<h2>写入性能</h2><p class="muted">p50/p99/p999：每组 {settings['latency_batch_size']:,} 次写入的平均耗时分位数，再对 {settings['repetitions']} 次运行取平均；不是单条端到端延时。
不扣除计时器开销。吞吐与延时分别运行、分别汇总，全部重复保存在原始数据中。</p>
<div class="scroll"><table><thead><tr><th>writer 单块</th><th>M条/s</th><th>GB/s</th><th>p50 ns</th><th>p99 ns</th><th>p999 ns</th><th>吞吐范围 M条/s</th><th>峰值 RSS MiB</th></tr></thead><tbody>{''.join(performance)}</tbody></table></div>
<h2>取块、换块与池深度</h2><p class="muted">取块次数、自然换块次数、完成块数是每次吞吐运行的平均值。完成块数包含轮末尾块，自然换块不包含轮末 finish_block。
池最大值取 {settings['repetitions']} 次最大；均值按全部成功取块样本加权，采样发生在新块借出之后、旧块归还之前。因此小块的取块平均占用会接近 2，归还完成后实际只剩当前块被借出。
两种策略的取块和换块次数必须一致。已分配容量包括空闲块，与实际 RSS 分开统计。</p>
<div class="scroll"><table><thead><tr><th>writer 单块</th><th>取块/次</th><th>自然换块/次</th><th>完成块/次</th><th>最大占用块数</th><th>平均占用块数</th><th>最大分配块数</th><th>平均分配块数</th><th>最大分配 MiB</th><th>背压/次</th></tr></thead><tbody>{''.join(depth)}</tbody></table></div>
<div class="card" style="margin-top:24px"><details><summary>环境、命令与源文件指纹</summary><pre>{escaped}</pre></details></div></main></html>'''
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(page, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report-prefix', type=Path)
    parser.add_argument('--min-block-bytes', type=int, default=1024)
    parser.add_argument('--max-block-bytes', type=int, default=GIB)
    parser.add_argument('--writer-cache-bytes', type=int, default=2*GIB)
    parser.add_argument('--initial-pool-blocks', type=int, default=2)
    parser.add_argument('--messages', type=int, default=10_000_000)
    parser.add_argument('--warmup-messages', type=int, default=50_000)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--round-storage-bytes', type=int, default=0, help='0=80% of the reader capacity reference')
    parser.add_argument('--reader-cache-bytes', type=int, default=2*GIB)
    parser.add_argument('--reader-block-multiplier', type=int, default=10)
    parser.add_argument('--latency-batch-size', type=int, default=1000)
    parser.add_argument('--producer-cpu', type=int, default=-1)
    parser.add_argument('--producer-qos', choices=('user_interactive', 'unchanged'),
                        default='user_interactive' if sys.platform == 'darwin' else 'unchanged')
    parser.add_argument('--timeout-seconds', type=int, default=180)
    parser.add_argument('--order-seed', type=int, default=1729)
    args = parser.parse_args()
    if (args.min_block_bytes < 32 or args.min_block_bytes & (args.min_block_bytes-1) or
        args.max_block_bytes < args.min_block_bytes or args.max_block_bytes & (args.max_block_bytes-1)):
        parser.error('block bounds must be ordered powers of two, at least 32 bytes')
    if (min(args.writer_cache_bytes, args.initial_pool_blocks, args.messages, args.repetitions,
            args.reader_cache_bytes, args.reader_block_multiplier, args.latency_batch_size, args.timeout_seconds) <= 0 or
        args.warmup_messages < 0 or args.producer_cpu < -1):
        parser.error('invalid positive counts/capacities, warmup or CPU')
    if args.writer_cache_bytes//args.max_block_bytes < max(2, args.initial_pool_blocks):
        parser.error('writer pool must fit the initial blocks and at least two maximum-size blocks')
    if args.round_storage_bytes < 0:
        parser.error('round storage override must be nonnegative')
    smallest_round = min(round_bytes_for(s,args.reader_block_multiplier,args.reader_cache_bytes,args.round_storage_bytes)
                         for s in (args.min_block_bytes,args.max_block_bytes))
    if min(smallest_round//32, args.messages) < args.latency_batch_size:
        parser.error('a latency round must fit one group')
    args.binary = args.binary.resolve()
    args.output = args.output.resolve()
    prefix = (args.report_prefix or args.output/'test_per_write_block_size_only_writer').resolve()
    reports = {1: Path(str(prefix)+'_publish1.html'), MAX_PUBLISH: Path(str(prefix)+'_publish_max.html')}
    args.output.mkdir(parents=True, exist_ok=False)
    build = json.loads(subprocess.check_output([str(args.binary), '--build-info'], text=True))
    if not build['optimized'] or build['sanitized'] or not build['single_thread'] or not build['pool_depth_instrumented']:
        raise ValueError('use an optimized, unsanitized, single-thread pool-instrumented executable')
    power = probe(['pmset', '-g', 'batt']) if sys.platform == 'darwin' else None
    if power and 'AC Power' not in power['stdout']:
        raise RuntimeError('connect AC power before the sweep')
    sources = [ROOT/'CMakeLists.txt', ROOT/'benchmarks/spsc.cmake', Path(__file__).resolve(),
               ROOT/'benchmarks/test_per_writer_block_size.py',
               ROOT/'benchmarks/test_per_write_block_size_only_writer.cpp',ROOT/'benchmarks/block_size_round_budget.hpp']
    for directory in ('src', 'include', 'benchmarks/tunable'):
        sources += [x for x in (ROOT/directory).rglob('*') if x.is_file() and x.suffix in ('.cpp', '.hpp')]
    settings = {k:v for k,v in vars(args).items() if k not in ('binary', 'output', 'report_prefix')}
    environment = dict(timestamp=datetime.datetime.now().astimezone().isoformat(), platform=platform.platform(),
                       machine=platform.machine(), build=build, binary=str(args.binary), binary_sha256=sha(args.binary),
                       settings=settings, power_before=power, memory_before=memory_state(),
                       source_sha256={str(x.relative_to(ROOT)):sha(x) for x in sources},
                       measurement='single-thread ordinary 32 B Writer, immediate completed-block recycle, no payload read during measurement',
                       publish_counts=[1, MAX_PUBLISH])
    save(args.output/'environment.json', environment)
    sizes = []
    size = args.min_block_bytes
    while size <= args.max_block_bytes:
        sizes.append(size)
        size *= 2
    rows = []
    started = time.monotonic()
    total = len(sizes)*2*(2*args.repetitions+1)
    with (args.output/'raw.jsonl').open('w', encoding='utf-8') as raw:
        def run(publish, size, mode, trial, stage):
            parameters = dict(source_block_size=size, writer_cache_bytes=args.writer_cache_bytes,
                              initial_pool_blocks=args.initial_pool_blocks, messages=args.messages,
                              warmup_messages=args.warmup_messages, publish_count=publish,
                              mode=mode, verify_payload=stage == 'validation',
                              reader_cache_bytes=args.reader_cache_bytes, reader_block_multiplier=args.reader_block_multiplier,
                              round_storage_bytes=round_bytes_for(size,args.reader_block_multiplier,
                                                                 args.reader_cache_bytes,args.round_storage_bytes),
                              latency_batch_size=args.latency_batch_size, producer_cpu=args.producer_cpu,
                              producer_qos=args.producer_qos, timeout_seconds=args.timeout_seconds)
            command = [str(args.binary)]
            for key, value in parameters.items():
                command += ['--'+key, str(value).lower() if isinstance(value, bool) else str(value)]
            process = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout_seconds+20)
            stem = f'{publish}_{size}_{stage}_{mode}_{trial}'
            (args.output/(stem+'.stdout.json')).write_text(process.stdout, encoding='utf-8')
            (args.output/(stem+'.stderr.txt')).write_text(process.stderr, encoding='utf-8')
            if process.returncode:
                save(args.output/'failure.json', dict(command=command, exit_code=process.returncode, stderr=process.stderr))
                raise RuntimeError(f'{stem}: {process.stderr}')
            result = json.loads(process.stdout)
            config = result['config']
            assert config['payload_bytes'] == 32 and config['source_block_size'] == size
            assert config['publish_count'] == publish and config['messages'] == args.messages
            assert result['single_thread'] and result['reader_threads'] == 0 and result['callback_threads'] == 0
            assert result['all_released'] and result['retired_messages'] == args.messages
            assert result['failed_write_calls'] == result['writer_pressure_events'] == 0
            assert config['verify_payload'] == (stage == 'validation')
            assert stage != 'validation' or result['verified']
            assert args.producer_qos != 'user_interactive' or result['producer_qos_applied']
            row = dict(publish_count=publish, writer_block_bytes=size, mode=mode, trial=trial, stage=stage,
                       command=command, result=result)
            rows.append(row)
            raw.write(json.dumps(row, ensure_ascii=False)+'\n')
            raw.flush()
            print(f'{len(rows):3d}/{total} {stage:11s} publish={publish:<10d} {size_label(size):>8s} '
                  f'{mode:13s} {result["m_messages_per_second"]:8.2f} M/s '
                  f'depth={result["pool_depth"]["writer"]["max_in_use"]} elapsed={time.monotonic()-started:.0f}s', flush=True)
        for size in sizes:
            for publish in (1, MAX_PUBLISH):
                run(publish, size, 'throughput', 0, 'validation')
        for trial in range(args.repetitions):
            jobs = [(publish, size, mode) for publish in (1, MAX_PUBLISH) for size in sizes
                    for mode in ('throughput', 'write_latency')]
            random.Random(args.order_seed+trial).shuffle(jobs)
            for publish, size, mode in jobs:
                run(publish, size, mode, trial, 'measurement')
            save(args.output/'summary.json', summarize(rows, args.repetitions))
    summary = summarize(rows, args.repetitions)
    # Both strategies must have identical acquisition/handoff counts, not just equal message counts.
    for size in sizes:
        pair = [r for r in summary if r['writer_block_bytes'] == size]
        assert len(pair) == 2
        for key in ('completed_blocks', 'natural_switches', 'rounds'):
            assert pair[0][key] == pair[1][key]
        assert pair[0]['writer_pool']['samples'] == pair[1]['writer_pool']['samples']
    environment['power_after'] = probe(['pmset', '-g', 'batt']) if sys.platform == 'darwin' else None
    environment['memory_after'] = memory_state()
    environment['elapsed_seconds'] = time.monotonic()-started
    environment['completed_runs'] = len(rows)
    environment['validation_runs'] = len(sizes)*2
    save(args.output/'environment.json', environment)
    save(args.output/'summary.json', summary)
    columns = ['publish_count', 'writer_block_bytes', 'throughput_mps', 'payload_gb_per_second',
               'p50_ns', 'p99_ns', 'p999_ns', 'peak_rss_mib', 'completed_blocks', 'natural_switches',
               'writer_max_depth', 'writer_average_depth', 'writer_depth_samples']
    with (args.output/'summary.csv').open('w', newline='', encoding='utf-8') as file:
        writer = csv.DictWriter(file, fieldnames=columns)
        writer.writeheader()
        for row in summary:
            value = {key:row[key] for key in columns[:10]}
            value.update(writer_max_depth=row['writer_pool']['max_in_use'],
                         writer_average_depth=row['writer_pool']['average_in_use'],
                         writer_depth_samples=row['writer_pool']['samples'])
            writer.writerow(value)
    for publish, report in reports.items():
        write_report(report, [r for r in summary if r['publish_count'] == publish], environment, publish,
                     reports[MAX_PUBLISH if publish == 1 else 1])
        print('Saved:', report, flush=True)


if __name__ == '__main__':
    main()
