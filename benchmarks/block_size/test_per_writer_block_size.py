"""Isolated 32 B ordinary-write block-size sweep using the existing C++ harness."""
from pathlib import Path
import argparse
import csv
import datetime
import hashlib
import html
import json
import platform
import random
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
GIB = 1024 ** 3


def reader_capacity_bytes(writer_block, multiplier, requested_bytes, fixed=False):
    reader_block = min(writer_block*multiplier,requested_bytes) if fixed else writer_block*multiplier
    return (requested_bytes if fixed else max(requested_bytes,reader_block))//reader_block*reader_block


def round_bytes_for(writer_block, multiplier, requested_bytes, override=0, fixed=False):
    limit = reader_capacity_bytes(writer_block,multiplier,requested_bytes,fixed)*4//5
    if override < 0 or override > limit:
        raise ValueError("round exceeds 80% of actual reader capacity")
    return override or limit


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def probe(command):
    result = subprocess.run(command, capture_output=True, text=True)
    return dict(command=command, exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)


def memory_state():
    if sys.platform == "darwin":
        return {"vm_stat": probe(["vm_stat"]), "swap": probe(["sysctl", "vm.swapusage"])}
    return {}


def pool_summary(results, side):
    values = [x["pool_depth"][side] for x in results]
    count = sum(x["samples"] for x in values)
    return dict(samples=count,
                max_in_use=max(x["max_in_use"] for x in values),
                average_in_use=sum(x["in_use_sum"] for x in values)/count if count else None,
                max_allocated=max(x["max_allocated"] for x in values),
                average_allocated=sum(x["allocated_sum"] for x in values)/count if count else None,
                average_free=sum(x["free_sum"] for x in values)/count if count else None,
                max_in_use_bytes=max(x["max_in_use_bytes"] for x in values),
                max_allocated_bytes=max(x["max_allocated_bytes"] for x in values),
                runs=values)


def summarize(rows, repetitions):
    output = []
    sizes = sorted({x["writer_block_bytes"] for x in rows})
    for size in sizes:
        measured = [x for x in rows if x["writer_block_bytes"] == size and x["stage"] == "measurement"]
        throughput = [x["packet"]["result"] for x in measured if x["mode"] == "throughput"]
        latency = [x["packet"]["result"] for x in measured if x["mode"] == "write_latency"]
        if len(throughput) != repetitions or len(latency) != repetitions:
            continue
        reference = measured[0]["packet"]
        result = dict(writer_block_bytes=size, reader_block_bytes=throughput[0]["config"]["output_block_size"],
                      block_size_test=reference["block_size_test"], parameters=throughput[0]["config"],
                      throughput_mps=statistics.fmean(x["m_messages_per_second"] for x in throughput),
                      throughput_runs_mps=[x["m_messages_per_second"] for x in throughput],
                      payload_gb_per_second=statistics.fmean(x["payload_gb_per_second"] for x in throughput),
                      peak_rss_mib=max(x["peak_rss_mib"] for x in throughput+latency),
                      writer_pool=pool_summary(throughput,"writer"), reader_pool=pool_summary(throughput,"reader"),
                      latency_writer_pool=pool_summary(latency,"writer"), latency_reader_pool=pool_summary(latency,"reader"))
        for metric in ("producer_seconds", "callback_processing_seconds", "callback_to_producer_ratio", "drain_seconds",
                       "failed_write_calls", "backpressured_messages", "producer_retry_seconds", "cpu_cores"):
            result[metric] = statistics.fmean(x[metric] for x in throughput)
        for metric in ("p50", "p99", "p999"):
            result[metric+"_ns"] = statistics.fmean(x["batch_average_write_ns"][metric] for x in latency)
            result[metric+"_runs_ns"] = [x["batch_average_write_ns"][metric] for x in latency]
        result["latency_samples"] = [x["latency_samples"] for x in latency]
        result["writer_pressure_events"] = statistics.fmean(x["library_backpressure"]["writer_pressure_events"] for x in throughput)
        result["copy_memory_full_events"] = statistics.fmean(x["library_backpressure"]["copy_memory_full_events"] for x in throughput)
        output.append(result)
    return output


def size_label(value):
    for divisor, unit in ((GIB,"GiB"),(1024**2,"MiB"),(1024,"KiB")):
        if value >= divisor:
            return f"{value/divisor:g} {unit}"
    return f"{value} B"


def number(value, places=3):
    return "—" if value is None else f"{value:,.{places}f}"


def write_report(path, rows, environment):
    if not rows:
        return
    fastest = max(rows, key=lambda x:x["throughput_mps"])["writer_block_bytes"]
    lowest = min(rows, key=lambda x:x["p50_ns"])["writer_block_bytes"]
    performance, depth, allocated = [], [], []
    for row in rows:
        label = html.escape(size_label(row["writer_block_bytes"]))
        reader = html.escape(size_label(row["reader_block_bytes"]))
        classes = ' class="best"' if row["writer_block_bytes"] == fastest else ""
        p50_class = ' class="metric"' if row["writer_block_bytes"] == lowest else ""
        rr = row["throughput_runs_mps"]
        performance.append(f'<tr{classes}><th>{label}</th><td>{reader}</td>'
                           f'<td>{number(row["throughput_mps"])}</td><td>{number(row["payload_gb_per_second"])}</td>'
                           f'<td{p50_class}>{number(row["p50_ns"])}</td><td>{number(row["p99_ns"])}</td>'
                           f'<td>{number(row["p999_ns"])}</td><td>{number(min(rr))}–{number(max(rr))}</td>'
                           f'<td>{number(row["callback_to_producer_ratio"],2)}</td><td>{number(row["peak_rss_mib"],1)}</td></tr>')
        w, r = row["writer_pool"], row["reader_pool"]
        depth.append(f'<tr><th>{label}</th><td>{w["max_in_use"]:,}</td><td>{number(w["average_in_use"])}</td>'
                     f'<td>{number(w["max_in_use_bytes"]/1024**2,2)}</td><td>{w["samples"]:,}</td>'
                     f'<td>{r["max_in_use"]:,}</td><td>{number(r["average_in_use"])}</td>'
                     f'<td>{number(r["max_in_use_bytes"]/1024**2,2)}</td><td>{r["samples"]:,}</td>'
                     f'<td>{number(row["writer_pressure_events"],1)}</td><td>{number(row["copy_memory_full_events"],1)}</td></tr>')
        allocated.append(f'<tr><th>{label}</th><td>{w["max_allocated"]:,}</td><td>{number(w["average_allocated"])}</td>'
                         f'<td>{number(w["max_allocated_bytes"]/1024**2,2)}</td><td>{r["max_allocated"]:,}</td>'
                         f'<td>{number(r["average_allocated"])}</td><td>{number(r["max_allocated_bytes"]/1024**2,2)}</td>'
                         f'<td>{size_label(row["block_size_test"]["reader_cache_effective_limit_bytes"])}</td></tr>')
    settings = environment["settings"]
    round_budget_note = (f"每轮上限 {size_label(settings['round_storage_bytes'])}，不超过 reader 实际总容量的 80%。"
                         if settings['round_storage_bytes'] else
                         "每轮上限按 reader 实际总容量的 80% 计算，不按 writer 单块或 writer 总容量计算。")
    scheduling = (f"写线程绑定 CPU {settings['producer_cpu']}。" if settings['producer_cpu'] >= 0 else "未硬绑核。")
    scheduling += "采用高 QoS 调度偏好。" if settings['producer_qos'] == "user_interactive" else "保留平台默认调度。"
    scan_current = settings.get("is_scan_current", True)
    scan_label = "当前块扫描 1 ms" if scan_current else "不扫描当前块"
    publish_count = settings.get("publish_count", 1)
    publish_label = "仅交接块时发布 · UINT32_MAX" if publish_count == 4294967295 else "每条 release 发布"
    round_note = ("" if scan_current else
                  "关闭 current 扫描时，沿用框架在每轮计时结束后 finish_block 交接尾块并排空；交接与排空时间不计入写入吞吐，下一轮申请新块仍计时。发布次数见上方标签。")
    escaped = html.escape(json.dumps(environment,ensure_ascii=False,indent=2))
    page = f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TwoBufQueue · writer block size 遍历</title>
<style>body{{margin:0;background:#f4f7fb;color:#17243b;font:15px/1.65 system-ui,-apple-system,sans-serif}}main{{max-width:1450px;margin:auto;padding:32px 24px}}h1{{font-size:29px;margin:0 0 12px}}h2{{margin:28px 0 10px;font-size:20px}}.intro,.card{{background:white;padding:22px;border:1px solid #dce5ef;border-radius:12px;margin-bottom:18px}}.scroll{{overflow:auto;border:1px solid #dce5ef;border-radius:10px}}table{{border-collapse:collapse;width:100%;white-space:nowrap;font-variant-numeric:tabular-nums}}td,th{{padding:11px 13px;text-align:right;border-bottom:1px solid #dce5ef}}thead th{{background:#213b60;color:white;font-size:13px}}tbody th{{text-align:left}}tbody tr{{background:#edf5ff}}tbody tr:nth-child(even){{background:#e5effc}}tbody tr:hover{{background:#d5e7fb}}tr.best{{background:#d5f0e7!important;font-weight:650}}.metric{{color:#076644;font-weight:750}}.muted{{color:#53667f}}.tag{{display:inline-block;background:#e5effc;padding:4px 10px;margin:0 7px 8px 0;border-radius:6px}}pre{{white-space:pre-wrap;font-size:12px;overflow:auto}}a{{color:#1766ab}}.bestnote{{color:#076644}}</style>
<main><h1>writer 块大小与热路径性能</h1><div class="intro">
<span class="tag">32 B</span><span class="tag">{publish_label}</span><span class="tag">{scan_label}</span>
<span class="tag">回调循环 {settings['callback_iterations']}</span><span class="tag">{settings['repetitions']} 次平均</span>
<p>每项发送 {settings['messages']:,} 条。reader 单块按 writer 的 {settings['reader_block_multiplier']} 倍计算，固定容量模式下单块不超过 reader 总上限，初始各 {settings['initial_pool_blocks']} 块（受池上限约束），随后按需增长。
writer 容量上限 {size_label(settings['writer_cache_bytes'])}；reader 上限 {size_label(settings['reader_cache_bytes'])}；固定容量模式不扩大上限，旧模式才允许扩容。{round_budget_note}</p>
<p class="muted">沿用 PerformanceTest 的数据准备、普通 write、两层复制、独立回调线程、分轮发送及排空逻辑。数据准备与轮间排空不计入写入吞吐；统计换块的开销包含在测量中。
{scheduling}没有添加目标读回或额外清零优化。</p>
<p class="muted">{round_note}</p>
<p class="bestnote">吞吐最高：{size_label(fastest)}；p50 最低：{size_label(lowest)}。所有重复均保留，未挑选最快一次。</p></div>
<h2>写入性能</h2><p class="muted">p50/p99/p999 是每组 {settings['latency_batch_size']:,} 次成功写入的平均耗时分位数，再对各次运行取平均；不是单条端到端延时。
回调/写入比为每轮首条至末条回调的时间与写入时间之比，包含分发和等待。RSS 是进程实际驻留内存峰值。</p>
<div class="scroll"><table><thead><tr><th>writer 单块</th><th>reader 单块</th><th>M条/s</th><th>GB/s</th><th>p50 ns</th><th>p99 ns</th><th>p999 ns</th><th>吞吐范围 M条/s</th><th>回调/写入比</th><th>峰值 RSS MiB</th></tr></thead><tbody>{''.join(performance)}</tbody></table></div>
<h2>占用深度：已借出、尚未归还的块</h2><p class="muted">每次成功取新块后，在原有池锁内取得一致计数。均值按取块次数加权；最大值包含正式阶段起始快照。这里汇总吞吐测试的全部重复，延时测试的独立深度数据保存在 JSON。
没有取块样本时均值显示 —。writer/reader 两侧独立采样，不代表同一时刻的联合快照。</p>
<div class="scroll"><table><thead><tr><th>writer 单块</th><th>writer 最大块数</th><th>writer 平均块数</th><th>writer 最大占用 MiB</th><th>writer 样本数</th><th>reader 最大块数</th><th>reader 平均块数</th><th>reader 最大占用 MiB</th><th>reader 样本数</th><th>writer 背压/次</th><th>复制层背压/次</th></tr></thead><tbody>{''.join(depth)}</tbody></table></div>
<h2>已分配块数与容量</h2><p class="muted">已分配包括借出和池内空闲块。下面的容量是块数 × 单块字节数，包含尚未触碰的虚拟内存，不等于实际 RSS。
平均值仍按成功取块次数加权。</p>
<div class="scroll"><table><thead><tr><th>writer 单块</th><th>writer 最大分配块数</th><th>writer 平均分配块数</th><th>writer 最大分配 MiB</th><th>reader 最大分配块数</th><th>reader 平均分配块数</th><th>reader 最大分配 MiB</th><th>reader 容量上限</th></tr></thead><tbody>{''.join(allocated)}</tbody></table></div>
<div class="card" style="margin-top:24px"><details><summary>环境、二进制和源文件指纹</summary><pre>{escaped}</pre></details></div></main></html>'''
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(page,encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--report",type=Path)
    parser.add_argument("--min-block-bytes",type=int,default=1024)
    parser.add_argument("--max-block-bytes",type=int,default=GIB)
    parser.add_argument("--reader-block-multiplier",type=int,default=10)
    parser.add_argument("--writer-cache-bytes",type=int,default=2*GIB)
    parser.add_argument("--reader-cache-bytes",type=int,default=2*GIB)
    parser.add_argument("--reader-cache-fixed",choices=("true","false"),default="true")
    parser.add_argument("--initial-pool-blocks",type=int,default=2)
    parser.add_argument("--messages",type=int,default=10_000_000)
    parser.add_argument("--warmup-messages",type=int,default=50_000)
    parser.add_argument("--callback-iterations",type=int,default=0)
    parser.add_argument("--is-scan-current",choices=("true","false"),default="true")
    parser.add_argument("--publish-count",type=int,choices=(1,4294967295),default=None)
    parser.add_argument("--repetitions",type=int,default=3)
    parser.add_argument("--round-storage-bytes",type=int,default=0,help="0=80%% of actual reader pool capacity")
    parser.add_argument("--latency-batch-size",type=int,default=1000)
    parser.add_argument("--producer-cpu",type=int,default=-1)
    parser.add_argument("--producer-qos",choices=("user_interactive","unchanged"),
                        default="user_interactive" if sys.platform == "darwin" else "unchanged")
    parser.add_argument("--timeout-seconds",type=int,default=180)
    parser.add_argument("--order-seed",type=int,default=1729)
    args = parser.parse_args()
    args.is_scan_current = args.is_scan_current == "true"
    args.reader_cache_fixed = args.reader_cache_fixed == "true"
    if args.publish_count is None:
        args.publish_count = 1 if args.is_scan_current else 4294967295
    if (args.min_block_bytes <= 0 or args.min_block_bytes & (args.min_block_bytes-1) or
        args.max_block_bytes < args.min_block_bytes or args.max_block_bytes & (args.max_block_bytes-1)):
        parser.error("block bounds must be ordered powers of two")
    if min(args.reader_block_multiplier,args.writer_cache_bytes,args.reader_cache_bytes,args.initial_pool_blocks,
           args.messages,args.repetitions,args.latency_batch_size,args.timeout_seconds) <= 0:
        parser.error("counts, capacities and multiplier must be positive")
    if args.round_storage_bytes < 0:
        parser.error("round storage override must be nonnegative")
    if args.warmup_messages < 0 or args.callback_iterations < 0:
        parser.error("warmup and callback iterations must be nonnegative")
    args.binary = args.binary.resolve()
    args.output = args.output.resolve()
    args.report = (args.report or args.output/"test_per_writer_block_size.html").resolve()
    args.output.mkdir(parents=True,exist_ok=False)
    build = json.loads(subprocess.check_output([str(args.binary),"--build-info"],text=True))
    if not build["optimized"] or build["sanitized"] or not build["pool_depth_instrumented"]:
        raise ValueError("use an optimized, unsanitized, pool-instrumented block-size executable")
    power = probe(["pmset","-g","batt"]) if sys.platform == "darwin" else None
    if power and "AC Power" not in power["stdout"]:
        raise RuntimeError("connect AC power before running the block-size sweep")
    settings = {k:v for k,v in vars(args).items() if k not in ("binary","output","report")}
    sources = [ROOT/"CMakeLists.txt",ROOT/"benchmarks/spsc/spsc.cmake",Path(__file__).resolve(),
               ROOT/"benchmarks/block_size/test_per_writer_block_size.cpp",ROOT/"benchmarks/block_size/block_size_round_budget.hpp"]
    for directory in ("src","include","benchmarks/tunable"):
        sources += [x for x in (ROOT/directory).rglob("*") if x.is_file() and x.suffix in (".cpp",".hpp")]
    environment = dict(timestamp=datetime.datetime.now().astimezone().isoformat(),platform=platform.platform(),
                       machine=platform.machine(),build=build,binary=str(args.binary),binary_sha256=sha(args.binary),
                       settings=settings,power_before=power,memory_before=memory_state(),
                       source_sha256={str(x.relative_to(ROOT)):sha(x) for x in sources},
                       dependencies=json.loads((ROOT/"benchmarks/spsc/spsc_dependencies.json").read_text(encoding="utf-8")),
                       measurement=("32 B ordinary write, publish_count=" + str(args.publish_count) + ", " +
                                    ("current scan 1 ms" if args.is_scan_current else "completed blocks only") +
                                    ", dynamic pools"))
    save(args.output/"environment.json",environment)
    rows = []
    sizes = []
    size = args.min_block_bytes
    while size <= args.max_block_bytes:
        sizes.append(size)
        size *= 2
    started = time.monotonic()
    with (args.output/"raw.jsonl").open("w",encoding="utf-8") as raw:
        def run(size,mode,trial,stage):
            parameters = dict(source_block_size=size,reader_block_multiplier=args.reader_block_multiplier,
                              writer_cache_bytes=args.writer_cache_bytes,reader_cache_bytes=args.reader_cache_bytes,
                              reader_cache_fixed=args.reader_cache_fixed,
                              initial_pool_blocks=args.initial_pool_blocks,messages=args.messages,
                              warmup_messages=args.warmup_messages,callback_iterations=args.callback_iterations,
                              is_scan_current=args.is_scan_current,publish_count=args.publish_count,
                              mode=mode,verify_payload=stage == "validation",
                              round_storage_bytes=round_bytes_for(size,args.reader_block_multiplier,
                                                                 args.reader_cache_bytes,args.round_storage_bytes,args.reader_cache_fixed),
                              latency_batch_size=args.latency_batch_size,producer_cpu=args.producer_cpu,
                              producer_qos=args.producer_qos,timeout_seconds=args.timeout_seconds,random_seed=42+trial)
            command = [str(args.binary)]
            for key,value in parameters.items():
                command += ["--"+key,str(value).lower() if isinstance(value,bool) else str(value)]
            process = subprocess.run(command,capture_output=True,text=True,timeout=args.timeout_seconds+20)
            stem = f"{size}_{stage}_{mode}_{trial}"
            (args.output/(stem+".stdout.json")).write_text(process.stdout,encoding="utf-8")
            (args.output/(stem+".stderr.txt")).write_text(process.stderr,encoding="utf-8")
            if process.returncode:
                save(args.output/"failure.json",dict(command=command,exit_code=process.returncode,stderr=process.stderr))
                raise RuntimeError(f"{stem}: {process.stderr}")
            packet = json.loads(process.stdout)
            result = packet["result"]
            config = result["config"]
            assert config["payload_bytes"] == 32 and config["publish_count"] == args.publish_count
            assert config["is_scan_current"] == args.is_scan_current
            assert config["source_block_size"] == size and config["output_block_size"] == (min(size*args.reader_block_multiplier,args.reader_cache_bytes) if args.reader_cache_fixed else size*args.reader_block_multiplier)
            assert result["consumed"] == result["timed_callback_messages"] == args.messages
            assert result["timing_scope"] == "producer_rounds" and result["seconds"] == result["producer_seconds"]
            assert result["pool_depth"]["writer"]["max_in_use"] <= config["source_pool_max"]
            assert result["pool_depth"]["reader"]["max_in_use"] <= config["output_pool_max"]
            if args.producer_qos == "user_interactive":
                assert result["producer_qos_applied"]
            if mode == "write_latency":
                assert result["latency_samples"]*args.latency_batch_size+result["latency_tail_writes"] == args.messages
                assert result["migrated_samples"] == 0
            row = dict(writer_block_bytes=size,mode=mode,trial=trial,stage=stage,command=command,packet=packet)
            rows.append(row)
            raw.write(json.dumps(row,ensure_ascii=False)+"\n")
            raw.flush()
            print(f"{len(rows):3d}/{len(sizes)*(2*args.repetitions+1)} {stage:11s} {size_label(size):>8s} "
                  f"{mode:13s} {result['m_messages_per_second']:8.2f} M/s "
                  f"depth={result['pool_depth']['writer']['max_in_use']}/{result['pool_depth']['reader']['max_in_use']} "
                  f"RSS={result['peak_rss_mib']:.0f} MiB elapsed={time.monotonic()-started:.0f}s",flush=True)
        for size in sizes:
            run(size,"throughput",0,"validation")
        for trial in range(args.repetitions):
            jobs = [(size,mode) for size in sizes for mode in ("throughput","write_latency")]
            random.Random(args.order_seed+trial).shuffle(jobs)
            for size,mode in jobs:
                run(size,mode,trial,"measurement")
            summary = summarize(rows,args.repetitions)
            save(args.output/"summary.json",summary)
    summary = summarize(rows,args.repetitions)
    environment["power_after"] = probe(["pmset","-g","batt"]) if sys.platform == "darwin" else None
    environment["memory_after"] = memory_state()
    environment["elapsed_seconds"] = time.monotonic()-started
    environment["completed_runs"] = len(rows)
    save(args.output/"environment.json",environment)
    save(args.output/"summary.json",summary)
    columns = ["writer_block_bytes","reader_block_bytes","throughput_mps","payload_gb_per_second",
               "p50_ns","p99_ns","p999_ns","peak_rss_mib","writer_max_depth","writer_average_depth",
               "reader_max_depth","reader_average_depth","writer_depth_samples","reader_depth_samples"]
    with (args.output/"summary.csv").open("w",newline="",encoding="utf-8") as file:
        writer = csv.DictWriter(file,fieldnames=columns)
        writer.writeheader()
        for row in summary:
            value = {key:row[key] for key in columns[:8]}
            for side in ("writer","reader"):
                stats = row[side+"_pool"]
                value.update({side+"_max_depth":stats["max_in_use"],side+"_average_depth":stats["average_in_use"],
                              side+"_depth_samples":stats["samples"]})
            writer.writerow(value)
    write_report(args.report,summary,environment)
    print("Saved:",args.report,flush=True)


if __name__ == "__main__":
    main()
