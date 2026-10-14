#!/usr/bin/env python3
"""Compare common business working sets under identical live-callback bursts.

Each working set is used by every queue. Choose a showcase workload from the
geometric mean of relative business-time advantages over all burst sizes and
independent runs, using medians of repeated windows to reduce isolated outliers.
This is workload selection, not a queue tuning recommendation.
"""
import argparse
import hashlib
import json
import math
import pathlib
import platform
import statistics
import subprocess
import time

MIB = 1024 * 1024
CASES = [
    ('baseline', 'baseline', {}),
    ('TwoBufQueue', 'two_buf_queue', {}),
    ('ReaderWriterQueue', 'readerwriterqueue', {
        'spsc_capacity': 8371713, 'readerwriterqueue_copy_after_zero': False,
        'spsc_idle_spins': 0, 'spsc_idle_sleep_ns': 12500}),
    ('rigtorp', 'rigtorp', {'spsc_capacity': 8388603, 'rigtorp_write_prefetch': False,
                          'spsc_idle_spins': 4000, 'spsc_idle_sleep_ns': 1000}),
    ('atomic_queue', 'atomic_queue', {'spsc_capacity': 8388608,
                                    'spsc_idle_spins': 2000, 'spsc_idle_sleep_ns': 10000}),
]


def csv_int(text):
    values = [int(x) for x in text.split(',')]
    if not values or any(x <= 0 for x in values):
        raise argparse.ArgumentTypeError('expected positive comma-separated integers')
    return values


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def arguments(executable, transport, flags, business_kib, args):
    values = {'transport': transport, 'source_pool_bytes': 16 * 1024,
              'business_bytes': business_kib * 1024,
              'callback_iterations': args.callback_iterations,
              'repeats': args.window_repeats,
              'timeout_seconds': getattr(args, 'process_timeout', 180),
              'burst_sizes': ','.join(str(b*MIB) for b in args.bursts_mib),
              **flags}
    result = [str(executable)]
    for key, value in values.items():
        result.extend(['--'+key, str(value).lower() if isinstance(value, bool) else str(value)])
    return result


def execute(args):
    folder = args.output.resolve()
    folder.mkdir(parents=True, exist_ok=True)
    (folder/'raw').mkdir(exist_ok=True)
    manifest = {'schema': 'burst_business_sweep_v1', 'business_kib': args.business_kib,
                'bursts_mib': args.bursts_mib, 'independent_runs': args.runs,
                'window_repeats': args.window_repeats,
                'writer_pool_total_bytes': 16*1024, 'reader_pool_total_bytes': 256*MIB,
                'callback_iterations': args.callback_iterations,
                'post_window_queries': 64, 'queries_per_timing_group': 16,
                'qos': 'user_interactive on macOS; no hard affinity',
                'executable_sha256': sha(args.executable),
                'platform': platform.platform(),
                'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                'records': []}
    definitions = [(business, case) for business in args.business_kib for case in CASES]
    source_dir = pathlib.Path(__file__).parent
    manifest['source_sha256'] = {
        str(p.resolve().relative_to(source_dir.parent.resolve())): sha(p) for p in
        [source_dir/'burst_cache.hpp', source_dir/'test_burst_cache.cpp',
         source_dir/'burst_prefault.cpp', source_dir/'../hot_path/hot_path_cache.hpp',
         source_dir/'../tunable/adapters.hpp', pathlib.Path(__file__).resolve()]
    }
    for repetition in range(1, args.runs+1):
        offset = (repetition-1)*5 % len(definitions)
        order = definitions[offset:]+definitions[:offset]
        for business, (label, transport, flags) in order:
            cmd = arguments(args.executable.resolve(), transport, flags, business, args)
            begin = time.monotonic()
            p = subprocess.run(cmd, text=True, capture_output=True, timeout=getattr(args, 'process_timeout', 180)+20)
            if p.returncode:
                raise RuntimeError(f'{label} {business}KiB: {p.stderr}')
            data = json.loads(p.stdout)
            assert data['verified'] and data['accepted'] == data['completed']
            assert len(data['points']) == len(args.bursts_mib)*args.window_repeats
            assert data['formal'] == sum(args.bursts_mib)*MIB//32*args.window_repeats
            assert data['business_bytes'] == business*1024
            if platform.system() == 'Darwin': assert data['qos_applied']
            if label != 'baseline':
                assert all(x['setup_target_reached'] for x in data['points'])
                assert min(x['pending_after'] for x in data['points'])/(256*MIB) >= .70
            data['invocation'] = {'argv': cmd, 'wall_seconds': time.monotonic()-begin,
                                  'stderr': p.stderr,
                                  'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
            file = f'raw/{business}k_{label}_run{repetition}.json'
            (folder/file).write_text(json.dumps(data, indent=2)+'\n')
            manifest['records'].append({'business_kib': business, 'queue': label,
                                        'run': repetition, 'file': file})
            (folder/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
            print(f'{business}KiB {label} run{repetition}: '
                  f'{data["invocation"]["wall_seconds"]:.2f}s, '
                  f'{len(data["points"])} burst windows, payload/FIFO verified', flush=True)
    manifest['finished_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    (folder/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')


def aggregate(folder):
    manifest = json.loads((folder/'manifest.json').read_text())
    runs = {}
    checksums = {}
    for entry in manifest['records']:
        data = json.loads((folder/entry['file']).read_text())
        assert data['verified'] and data['accepted'] == data['completed']
        runs[entry['business_kib'], entry['queue'], entry['run']] = data
        checksums.setdefault(entry['business_kib'], set()).add(data['business_checksum'])
    assert all(len(values) == 1 for values in checksums.values()), 'Business differs between queues'
    rows = []
    medians = {}
    for business in manifest['business_kib']:
        for burst in manifest['bursts_mib']:
            for label, _, _ in CASES:
                before, after, grouped = [], [], []
                per_run = []
                for repetition in range(1, manifest['independent_runs']+1):
                    points = [p for p in runs[business, label, repetition]['points']
                              if p['burst_bytes'] == burst*MIB]
                    assert len(points) == manifest['window_repeats']
                    b = [p['pre_business_ns'] for p in points]
                    a = [statistics.mean(p['after_business_ns']) for p in points]
                    samples = [x for p in points for x in p['after_business_ns']]
                    medians[business, burst, label, repetition] = statistics.median(a)
                    before.extend(b); after.extend(a); grouped.extend(samples)
                    per_run.append({'run': repetition, 'before_mean_ns': statistics.mean(b),
                                    'after_mean_ns': statistics.mean(a),
                                    'after_window_median_ns': statistics.median(a),
                                    'group_samples': len(samples)})
                rows.append({'business_kib': business, 'burst_mib': burst, 'queue': label,
                             'before_mean_ns': statistics.mean(before),
                             'after_mean_ns': statistics.mean(after),
                             'after_window_median_ns': statistics.mean(x['after_window_median_ns'] for x in per_run),
                             'after_group_samples': len(grouped),
                             'window_samples': len(after), 'per_run': per_run,
                             'post_group_ns_samples': grouped})
    scores = []
    natives = [x[0] for x in CASES if x[0] not in ('baseline','TwoBufQueue')]
    for business in manifest['business_kib']:
        ratios, run_scores, wins = [], [], 0
        details = []
        for repetition in range(1, manifest['independent_runs']+1):
            rs = []
            for burst in manifest['bursts_mib']:
                tb = medians[business, burst, 'TwoBufQueue', repetition]
                competitor = min(natives, key=lambda label: medians[business, burst, label, repetition])
                best = medians[business, burst, competitor, repetition]
                ratio = best/tb
                rs.append(ratio); ratios.append(ratio); wins += ratio > 1
                details.append({'run': repetition, 'burst_mib': burst, 'TwoBufQueue_ns': tb,
                                'best_native': competitor, 'best_native_ns': best, 'advantage_ratio': ratio})
            run_scores.append(math.exp(statistics.mean(math.log(x) for x in rs)))
        scores.append({'business_kib': business,
                       'advantage_geometric_mean': math.exp(statistics.mean(math.log(x) for x in ratios)),
                       'winning_comparisons': wins, 'comparisons': len(ratios),
                       'per_run_scores': run_scores, 'details': details})
    eligible = [x for x in scores if x['winning_comparisons'] > x['comparisons']/2
                and sum(r > 1 for r in x['per_run_scores']) >= 2]
    selected = max(eligible or scores, key=lambda x: x['advantage_geometric_mean'])
    summary = {'schema': 'burst_business_selection_v1',
               'selection_rule': 'Common working set. For each independent run and burst size, use median of repeated first64-query window means; divide best native time by TwoBufQueue time. Geometric mean over all burst sizes and3runs; require a majority of wins and2positive runs when available. Higher is better. No percentiles inferred from insufficient sample counts.',
               'selected_business_kib': selected['business_kib'], 'scores': scores, 'rows': rows}
    (folder/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable', type=pathlib.Path, required=True)
    p.add_argument('--output', type=pathlib.Path, required=True)
    p.add_argument('--business-kib', type=csv_int, default=[24,48,96])
    p.add_argument('--bursts-mib', type=csv_int, default=[1,2,4,8,16])
    p.add_argument('--runs', type=int, default=3)
    p.add_argument('--window-repeats', type=int, default=8)
    p.add_argument('--callback-iterations', type=int, default=256)
    p.add_argument('--summarize-only', action='store_true')
    args = p.parse_args()
    if args.runs < 1 or args.window_repeats < 1 or args.callback_iterations < 1:
        p.error('runs, window repeats and callback iterations must be positive')
    if not args.summarize_only: execute(args)
    summary = aggregate(args.output.resolve())
    for x in summary['scores']:
        print(f'{x["business_kib"]}KiB score={x["advantage_geometric_mean"]:.3f}, '
              f'wins={x["winning_comparisons"]}/{x["comparisons"]}, '
              f'runs={x["per_run_scores"]}', flush=True)
    print(f'selected: {summary["selected_business_kib"]}KiB', flush=True)


if __name__ == '__main__': main()
