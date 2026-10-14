#!/usr/bin/env python3
"""Search three Writer APIs using the same PerformanceTest as native SPSC comparisons.

Reader bytes and round bytes are invariants. Block and pool parameters are the
only differences between candidates; prepared data, timing and validation are
provided by tune_spsc.ParameterSearch, not a second benchmark implementation.
"""
import argparse
import hashlib
import html
import json
import subprocess
import sys
from pathlib import Path
import tune_spsc as tune

KIB, MIB, GIB = 1024, 1024**2, 1024**3
MODES = {
    "session": dict(use_write_session=True, publish_count=2**32-1, is_scan_current=False),
    "publish1": dict(use_write_session=False, publish_count=1, is_scan_current=True),
    "publish_max": dict(use_write_session=False, publish_count=2**32-1, is_scan_current=False),
}
LABELS = {"session": "Session", "publish1": "普通 write：每条 publish", "publish_max": "普通 write：publish MAX"}


def mode_of(config):
    return "session" if config["use_write_session"] else "publish1" if config["publish_count"] == 1 else "publish_max"


def with_pools(base, writer_block, writer_bytes, reader_block, reader_bytes):
    if writer_bytes % writer_block or reader_bytes % reader_block or reader_block < writer_block:
        raise ValueError("blocks must divide their pools exactly; reader block must fit writer block")
    writer_count, reader_count = writer_bytes//writer_block, reader_bytes//reader_block
    if writer_count < 2 or reader_count < 1:
        raise ValueError("writer pool must fit two blocks")
    return dict(base, source_block_size=writer_block, source_pool_min=writer_count,
                source_pool_max=writer_count, output_block_size=reader_block,
                output_pool_min=reader_count, output_pool_max=reader_count,
                source_queue_capacity=max(512, writer_count), output_queue_capacity=max(512, reader_count),
                buffer_budget_bytes=writer_bytes+reader_bytes)


def assert_invariants(c, reader_bytes, round_bytes):
    assert c["output_block_size"]*c["output_pool_max"] == reader_bytes
    assert c["output_pool_min"] == c["output_pool_max"]
    assert c["source_pool_min"] == c["source_pool_max"]
    assert c["source_pool_max"]*c["source_block_size"] <= 2*GIB
    assert c["round_storage_bytes"] == round_bytes == reader_bytes*4//5
    assert c["fixed_round_boundaries"] and c["prepared_input"] and c["variable_payload"]
    assert c["latency_batch_size"] == 1000 and c["callback_iterations"] == 0
    assert all(c[key] == value for key, value in MODES[mode_of(c)].items())


def unique(configs):
    return list({tune.candidate_id(c): c for c in configs}.values())


def group_candidates(candidates, length, mode):
    return [c for c in candidates if c["parameters"]["payload_bytes"] == length and mode_of(c["parameters"]) == mode]


def blocks_candidates(base, lengths, reader_bytes, modes=None):
    result = []
    for length in lengths:
        for mode in (modes or MODES):
            for exponent in range(10, 31):
                writer = 1 << exponent
                writer_bytes = max(8*MIB, 2*writer)
                reader = min(reader_bytes, max(MIB, writer*16))
                c = with_pools(dict(base, payload_bytes=length, **MODES[mode]), writer, writer_bytes, reader, reader_bytes)
                assert_invariants(c, reader_bytes, reader_bytes*4//5)
                result.append(c)
    return unique(result)


def refinement_candidates(screened, lengths, reader_bytes, modes=None):
    result = []
    for length in lengths:
        for mode in (modes or MODES):
            group = group_candidates(screened, length, mode)
            parents = unique([tune.select_best(group, objective)["parameters"] for objective in tune.OBJECTIVES])
            for c in parents:
                writer = c["source_block_size"]
                writer_bytes = writer*c["source_pool_max"]
                reader = c["output_block_size"]
                for budget in (2*writer, max(64*MIB, 2*writer)):
                    result.append(with_pools(c, writer, budget, reader, reader_bytes))
                for multiplier in (1, 256):
                    block = min(reader_bytes, max(64*KIB, writer*multiplier))
                    result.append(with_pools(c, writer, writer_bytes, block, reader_bytes))
                for key, values in {"output_frame_capacity": (1, 64),
                                    "reader_spin_count": (0, 10000),
                                    "rescan_interval_ns": (5_000_000, 30_000_000)}.items():
                    for value in values:
                        changes = {key: value}
                        if key == "rescan_interval_ns":
                            changes["rescan_max_interval_ns"] = value
                        result.append(dict(c, **changes))
    return unique(result)


def combinations_candidates(screened, lengths, modes=None):
    """Test combinations of the separately winning coordinates without a huge grid."""
    result = []
    axes = (("source_pool_min", "source_pool_max", "source_queue_capacity"),
            ("output_block_size", "output_pool_min", "output_pool_max", "output_queue_capacity"),
            ("output_frame_capacity",), ("reader_spin_count",),
            ("rescan_interval_ns", "rescan_max_interval_ns"))
    all_keys = {k for axis in axes for k in axis}
    for length in lengths:
        for mode in (modes or MODES):
            group = group_candidates(screened, length, mode)
            parents = unique([tune.select_best(group, o)["parameters"] for o in tune.OBJECTIVES])
            for parent in parents:
                compatible = [c for c in group if c["parameters"]["source_block_size"] == parent["source_block_size"]]
                for objective in tune.OBJECTIVES:
                    combo = dict(parent)
                    for axis in axes:
                        matches = [c for c in compatible if all(
                            c["parameters"][k] == parent[k] for k in all_keys-set(axis))]
                        if matches:
                            best = tune.select_best(matches, objective)["parameters"]
                            for key in axis:
                                combo[key] = best[key]
                    combo["buffer_budget_bytes"] = combo["source_block_size"]*combo["source_pool_max"] + combo["output_block_size"]*combo["output_pool_max"]
                    result.append(combo)
    return unique(result)


def pressure_free(candidate, rows):
    runs=[r["result"] for r in rows if r["candidate_id"]==candidate["candidate_id"] and r["stage"]==candidate["stage"]]
    return bool(runs) and all(not r["failed_write_calls"] and
        not (r.get("library_backpressure") or {}).get("writer_pressure_events",0) for r in runs)


def safe_finalists(screened, rows, lengths, modes=None):
    configs=[]
    for length in lengths:
        for mode in (modes or MODES):
            group=[c for c in group_candidates(screened,length,mode) if pressure_free(c,rows)]
            if not group:
                raise RuntimeError(f"no writer-pressure-free screened candidate for {length}/{mode}")
            configs += [tune.select_best(group,o)["parameters"] for o in tune.OBJECTIVES]
    return unique(configs)


def render_report(output, candidates, winners, audit):
    """One row per complete configuration; exactly seven visible columns."""
    def size(value):
        for unit, label in ((GIB, "GiB"), (MIB, "MiB"), (KIB, "KiB")):
            if value >= unit and value % unit == 0:
                return f"{value // unit} {label}"
        return f"{value:,} B"

    # Keep one measurement set, rather than pooling screening and confirmation.
    selected = {}
    for candidate in candidates:
        identity = candidate["candidate_id"]
        previous = selected.get(identity)
        if previous is None or candidate["stage"] == "final":
            selected[identity] = candidate
    ordered = sorted(selected.values(), key=lambda c: (
        c["parameters"]["payload_bytes"], c["parameters"]["source_block_size"],
        c["parameters"]["output_block_size"], c["candidate_id"]))
    modes = audit.get("modes", list(MODES))
    headings = ("消息长度 (B)", "writer block size", "reader block size",
                "吞吐 (M条/s)", "p50 (ns/条)", "p99 (ns/条)", "p999 (ns/条)")
    header = "<thead><tr>" + "".join(f"<th>{h}</th>" for h in headings) + "</tr></thead>"

    def row(candidate):
        p, m = candidate["parameters"], candidate["metrics"]
        cells = (str(p["payload_bytes"]), size(p["source_block_size"]),
                 size(p["output_block_size"]), f'{m["throughput"]:.3f}',
                 f'{m["p50"]:.3f}', f'{m["p99"]:.3f}', f'{m["p999"]:.3f}')
        tooltip = (f'配置 {candidate["candidate_id"]}；数据阶段 {candidate["stage"]}\n'
                   f'吞吐范围 {m["throughput_min"]:.3f}～{m["throughput_max"]:.3f} M条/s\n'
                   + json.dumps(p, ensure_ascii=False, indent=2)
                   + "\n背压统计：" + json.dumps(m.get("library_backpressure"), ensure_ascii=False))
        attrs = (f'data-length="{p["payload_bytes"]}" data-writer="{p["source_block_size"]}" '
                 f'data-reader="{p["output_block_size"]}" data-candidate="{candidate["candidate_id"]}" '
                 f'data-stage="{candidate["stage"]}" class="b{p["payload_bytes"]}" '
                 f'title="{html.escape(tooltip, quote=True)}"')
        return f'<tr {attrs}>' + "".join(f"<td>{html.escape(v)}</td>" for v in cells) + "</tr>"

    tables = []
    for mode in modes:
        group = [c for c in ordered if mode_of(c["parameters"]) == mode]
        tables.append(f'<section class="box" data-mode="{mode}"><h2>{html.escape(LABELS[mode])}</h2>'
                      f'<p class="muted">{len(group)} 个完整参数组合</p><div class="scroll"><table>{header}'
                      + "<tbody>" + "".join(row(c) for c in group) + "</tbody></table></div></section>")
    readback = ("普通 write／write_mem 已加入首字节读回，Session 未加入。"
                if audit.get("extra_readback") else "本次未加入额外读回。")
    session_note = ("本次未重新测量 Session。" if "session" not in modes else "")
    resource_note = ("本机资源快照记录到全局换页活动；部分大块配置波动较大，三次吞吐范围可悬停查看。"
                     if audit.get("global_swap_pages_since_archive_snapshot") else "")
    details_link = (' · <a href="report_full_details_before_compact_20261011.html">原完整对照报告</a>'
                    if (output / "report_full_details_before_compact_20261011.html").exists() else "")
    payload = f'''<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TwoBufQueue 各参数性能结果</title>
<style>
body{{margin:0;padding:24px;background:#f5f7fb;color:#173049;font:15px/1.65 system-ui,sans-serif}}
main{{max-width:1150px;margin:auto}}h1{{font-size:28px;margin:0 0 12px}}h2{{font-size:20px;margin:0 0 4px}}
.box{{background:white;padding:20px;border-radius:12px;margin:16px 0}}p{{margin:8px 0}}.muted{{color:#5a6b7b}}
.scroll{{overflow:auto;max-height:72vh}}table{{border-collapse:collapse;width:100%;font-size:14px;font-variant-numeric:tabular-nums}}
th,td{{padding:10px 12px;border:1px solid #dbe3ec;white-space:nowrap;text-align:right}}
th{{position:sticky;top:0;background:#173049;color:white;z-index:1}}tbody tr:hover{{outline:2px solid #4979b2;outline-offset:-2px}}
.b8{{background:#e7f3ff}}.b16{{background:#e9f6ed}}.b32{{background:#fff2d9}}.b64{{background:#f2eafe}}
select{{font:inherit;padding:5px 8px;margin-right:12px}}a{{color:#1566ac}}details{{margin-top:10px}}[hidden]{{display:none!important}}
</style></head><body><main><div class="box"><h1>TwoBufQueue 各参数性能结果</h1>
<p>按 <b>消息长度 → writer block size → reader block size</b> 升序排列；发布策略分别列出。</p>
<p>每个完整参数组合一行，共 <b>{len(ordered)}</b> 行。已有独立复测的组合使用 final 数据，其余使用 screen 数据；两阶段不合并平均。
同一消息长度和块大小可能出现多行，因为第一层容量、自旋、帧容量等其他参数不同。<b>悬停任意行可查看完整参数和吞吐范围。</b></p>
<p class="muted">吞吐单位 M条/s；p50／p99／p999 单位 ns/条，是每组 1,000 次成功写入平均耗时的分位数，分别按 {audit['repetitions']} 次测量取平均。</p>
<details><summary>测试条件与原始数据</summary>
<p>第二层总容量固定 {size(audit['reader_bytes'])}；每轮字节上限固定 {audit['round_storage_bytes']:,} B（80%）。正式写入 {audit['messages']:,} 条，预热 50,000 条，回调循环 0。生产者高 QoS、未硬绑核；数据提前准备；仅累计生产者发送时间，尾块交接与回调排空在计时外。{readback}{session_note}</p>
<p>列表包含全部候选，未按速度或背压过滤；完整原始计数保留。{resource_note}</p>
<p><a href="all_candidates.json">全部阶段和参数 JSON</a> · <a href="raw.jsonl">每次测量原始数据</a> · <a href="best.json">各指标最优参数</a> · <a href="audit.json">测量审计</a>{details_link}</p>
</details></div><div class="box"><label>消息长度 <select id="length"><option value="">全部</option>{''.join(f'<option value="{n}">{n} B</option>' for n in audit['lengths'])}</select></label>
<label>发布策略 <select id="mode"><option value="">全部</option>{''.join(f'<option value="{m}">{html.escape(LABELS[m])}</option>' for m in modes)}</select></label></div>
{''.join(tables)}</main><script>
function filter(){{const l=document.getElementById('length').value,m=document.getElementById('mode').value;
document.querySelectorAll('tbody tr').forEach(r=>r.hidden=Boolean(l&&r.dataset.length!==l));
document.querySelectorAll('section[data-mode]').forEach(s=>s.hidden=Boolean(m&&s.dataset.mode!==m));}}
document.querySelectorAll('select').forEach(s=>s.addEventListener('change',filter));
</script></body></html>'''
    (output / "report.html").write_text(payload, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--modes", choices=tuple(MODES), nargs="+", default=list(MODES))
    parser.add_argument("--seed-results", type=Path, help="Include previous winner parameters for the selected modes")
    parser.add_argument("--lengths", type=int, nargs="+", default=[8,16,32,64])
    parser.add_argument("--reader-bytes", type=int, default=2*GIB)
    parser.add_argument("--messages", type=int, default=10_000_000)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--producer-cpu", type=int, default=-1)
    parser.add_argument("--producer-qos", choices=("unchanged","user_interactive"), default="user_interactive" if sys.platform=="darwin" else "unchanged")
    args = parser.parse_args()
    if args.reader_bytes < GIB or args.reader_bytes & (args.reader_bytes-1):
        parser.error("reader bytes must be a power of two >= 1 GiB")
    if args.repetitions < 3 or args.messages < 1000 or set(args.lengths)-{8,16,32,64}:
        parser.error("use supported lengths, at least 1,000 messages and at least 3 repetitions")
    if len(set(args.modes)) != len(args.modes):
        parser.error("modes must be distinct")
    round_bytes = args.reader_bytes*4//5
    profile = dict(payload_bytes=args.lengths, callback_iterations=[0], transports=["two_buf_queue"], spaces={},
        base=dict(warmup_messages=50_000, latency_batch_size=1000, round_storage_bytes=round_bytes,
                  fixed_round_boundaries=True, output_mode="spsc", output_reader_mode="wait_signal",
                  output_frame_capacity=1024, reader_spin_count=1000,
                  rescan_interval_ns=1_000_000, rescan_max_interval_ns=1_000_000,
                  publish_count=1, producer_qos=args.producer_qos, producer_cpu=args.producer_cpu, measure_callback_time=True,
                  prepared_input=True, variable_payload=True, prefault_producer_buffers=True,
                  prepared_input_limit_bytes=args.reader_bytes, timeout_seconds=180),
        search=dict(screen_messages=args.messages, final_messages=args.messages,
                    screen_repetitions=args.repetitions, final_repetitions=args.repetitions,
                    validation_messages=args.messages, batch_reference=False))
    search = tune.ParameterSearch(args.executable, profile, args.output)
    profile_path = args.output/"profile.json"
    profile_path.write_text(json.dumps(profile,indent=2)+"\n")
    tune.write_environment(search, profile_path)
    candidates, seen = [], set()
    stages = []
    def screen(configs, name):
        fresh = []
        for c in unique(configs):
            assert_invariants(c, args.reader_bytes, round_bytes)
            identity = tune.candidate_id(c)
            if identity not in seen:
                seen.add(identity); fresh.append(c)
        print(f"{name}: {len(fresh)} new configurations", flush=True)
        (args.output/f"{name}_configs.json").write_text(json.dumps(fresh,indent=2)+"\n")
        stages.append(dict(name=name, candidates=len(fresh)))
        candidates.extend(search.evaluate(fresh, "screen"))
        (args.output/"screened.json").write_text(json.dumps(candidates,ensure_ascii=False,indent=2)+"\n")
    screen(blocks_candidates(search.base,args.lengths,args.reader_bytes,args.modes), "block_grid")
    if args.seed_results:
        seed=json.loads(args.seed_results.read_text())["winners"]
        configs=[c["parameters"] for c in seed if mode_of(c["parameters"]) in args.modes
                 and c["parameters"]["payload_bytes"] in args.lengths]
        frozen=("warmup_messages","latency_batch_size","round_storage_bytes","fixed_round_boundaries",
                "callback_iterations","prepared_input","variable_payload","prefault_producer_buffers",
                "producer_cpu","producer_qos","measure_callback_time")
        for c in configs:
            if any(c[k]!=search.base[k] for k in frozen):
                raise ValueError("seed results used a different workload or environment")
        screen(configs,"previous_winners")
    screen(refinement_candidates(candidates,args.lengths,args.reader_bytes,args.modes), "coordinate_refinement")
    screen(combinations_candidates(candidates,args.lengths,args.modes), "combinations")
    finalists = []
    for length in args.lengths:
        for mode in args.modes:
            group = group_candidates(candidates, length, mode)
            for objective in tune.OBJECTIVES:
                finalists.append(tune.select_best(group,objective)["parameters"])
    finalists = unique(finalists)
    print(f"Independent confirmation: {len(finalists)} configurations", flush=True)
    confirmed = search.evaluate(finalists,"final")
    extra=[c for c in safe_finalists(candidates,search.rows,args.lengths,args.modes)
           if tune.candidate_id(c) not in {f["candidate_id"] for f in confirmed}]
    print(f"No Writer pressure confirmation: {len(extra)} additional configurations",flush=True)
    confirmed.extend(search.evaluate(extra,"final"))
    unrestricted=[]
    for length in args.lengths:
        for mode in args.modes:
            group=group_candidates(confirmed,length,mode)
            unrestricted += [dict(tune.objective_result(group,o,length,0),writer_mode=mode) for o in tune.OBJECTIVES]
    winners, groups = [], []
    for length in args.lengths:
        for mode in args.modes:
            group = [c for c in group_candidates(confirmed,length,mode) if pressure_free(c,search.rows)]
            if not group:
                raise RuntimeError(f"no confirmation results for {length}/{mode}")
            winners += [dict(tune.objective_result(group,o,length,0), writer_mode=mode) for o in tune.OBJECTIVES]
            groups.append(dict(payload_bytes=length,writer_mode=mode,tested_candidates=len(group_candidates(candidates,length,mode)),finalists=len(group)))
    all_candidates = candidates+confirmed
    for c in all_candidates:
        c["metrics"]["writer_pressure_free_all_runs"] = pressure_free(c,search.rows)
    (args.output/"all_candidates.json").write_text(json.dumps(all_candidates,ensure_ascii=False,indent=2)+"\n")
    search.write_reports(all_candidates,groups,winners)
    power = subprocess.run(["pmset","-g","batt"],capture_output=True,text=True).stdout if sys.platform=="darwin" else "unavailable"
    audit = dict(modes=args.modes, reader_bytes=args.reader_bytes,round_storage_bytes=round_bytes,lengths=args.lengths,
                 messages=args.messages,repetitions=args.repetitions,stages=stages,
                 screen_candidates=len(candidates),final_candidates=len(confirmed),
                 validation_runs=sum(r["stage"]=="validation" for r in search.rows),
                 measurement_runs=sum(r["stage"]!="validation" for r in search.rows),failed_runs=len(search.failures),
                 power=power,writer_changes=bool(search.build_info.get("ordinary_writer_readback",False)),
                 extra_readback=bool(search.build_info.get("ordinary_writer_readback",False)),
                 session_readback=bool(search.build_info.get("session_readback",False)),
                 seed_results=str(args.seed_results) if args.seed_results else None,
                 selection_constraint="every confirmed throughput/latency run has zero failed_write_calls and zero writer_pressure_events",
                 unrestricted_winners=unrestricted,
                 capacity_checks="exact reader bytes and exact nominal round bytes asserted for every candidate",
                 harness="same tunable/performance_test.hpp and Payload as native SPSC comparisons",
                 executable_sha256=hashlib.sha256(Path(args.executable).read_bytes()).hexdigest())
    (args.output/"audit.json").write_text(json.dumps(audit,ensure_ascii=False,indent=2)+"\n")
    render_report(args.output,all_candidates,winners,audit)
    search.raw.close(); search.errors.close()
    print(json.dumps(audit,ensure_ascii=False),flush=True)
    print("Saved:",args.output/"report.html",flush=True)


if __name__ == "__main__":
    main()
