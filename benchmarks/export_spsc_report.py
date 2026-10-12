#!/usr/bin/env python3
"""Export independently confirmed metric winners without running another benchmark."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import statistics

from spsc_html_report import write_html_report

OBJECTIVES = ('throughput', 'p50', 'p99', 'p999')
TRANSPORTS = ('two_buf_queue', 'readerwriterqueue', 'rigtorp', 'atomic_queue')


def assemble_report(directory, cache_mib, callback_policy):
    directory = Path(directory)
    best = json.loads((directory/'best.json').read_text())
    environment = json.loads((directory/'environment.json').read_text())
    all_rows = [json.loads(line) for line in (directory/'raw.jsonl').read_text().splitlines()]
    winners = best['winners'] + best['unresolved']
    repetitions = best['settings']['final_repetitions']
    messages = best['settings']['final_messages']
    lengths = environment['profile']['payload_bytes']
    if best['failed_runs']:
        raise ValueError('resolve failed search runs before publishing a report')
    if not environment['build']['optimized'] or environment['build']['sanitized']:
        raise ValueError('release benchmark required')
    cache_bytes = cache_mib * 1048576
    selected, groups, callback_counts = {}, [], {}
    for length in lengths:
        for transport in TRANSPORTS:
            choices, measured = {}, {}
            for objective in OBJECTIVES:
                matches = [w for w in winners if w['payload_bytes'] == length and
                           w['parameters']['transport'] == transport and w['objective'] == objective]
                if len(matches) != 1:
                    raise ValueError('each implementation requires one selection per objective')
                w = matches[0]
                choices[objective] = w
                mode = 'throughput' if objective == 'throughput' else 'write_latency'
                rows = sorted([r for r in all_rows if r['stage'] == 'final' and
                               r['candidate_id'] == w['candidate_id'] and
                               r['result']['config']['mode'] == mode], key=lambda r: r['trial'])
                if [r['trial'] for r in rows] != list(range(repetitions)):
                    raise ValueError('missing, duplicated or partial confirmation runs')
                for row in rows:
                    r, c = row['result'], row['result']['config']
                    if r['consumed'] != messages or c['messages'] != messages or r['timed_callback_messages'] != messages:
                        raise ValueError('formal message counts differ')
                    if not c['prepared_input'] or not c['variable_payload']:
                        raise ValueError('do not mix legacy input workloads into this report')
                    if r['seconds'] != r['producer_seconds'] or r['timing_scope'] != 'producer_rounds':
                        raise ValueError('producer timing scope differs')
                    if c['output_mode'] != 'spsc' or r['source_record_bytes'] != length:
                        raise ValueError('this report requires unframed asynchronous runs')
                    for prefix in ('source', 'output'):
                        if any(c[prefix+'_block_size']*c[prefix+'_pool_'+bound] != cache_bytes
                               for bound in ('min', 'max')):
                            raise ValueError('a tuning candidate changed the cache byte budget')
                    if c['spsc_capacity'] or c['spsc_buffer_bytes'] != cache_bytes:
                        raise ValueError('native queue request differs from the cache budget')
                    if c['producer_qos'] == 'user_interactive' and not r['producer_qos_applied']:
                        raise ValueError('requested QoS was not applied')
                    callback_counts.setdefault(length, set()).add(c['callback_iterations'])
                    key = (row['candidate_id'], mode, row['trial'])
                    selected.setdefault(key, dict(row, report_objectives=[]))['report_objectives'].append(objective)
                measured[objective] = [r['result'] for r in rows]
            through = measured['throughput']
            latency = [r for objective in OBJECTIVES[1:] for r in measured[objective]]
            average = lambda key: statistics.fmean(r[key] for r in through)
            internal = [r['library_backpressure'] for r in through]
            pressure = ({key: (statistics.fmean(p[key] for p in internal)
                              if isinstance(internal[0][key], (int, float)) else internal[0][key])
                         for key in internal[0]}
                        if all(p is not None for p in internal) else None)
            g = dict(payload_bytes=length, transport=transport,
                     callback_iterations=through[0]['config']['callback_iterations'],
                     throughput_mps=average('m_messages_per_second'),
                     payload_gb_per_second=average('payload_gb_per_second'),
                     throughput_runs_mps=[r['m_messages_per_second'] for r in through],
                     latency_runs=[{q: measured[q][trial]['batch_average_write_ns'][q] for q in OBJECTIVES[1:]}
                                   for trial in range(repetitions)],
                     internal_backpressure=pressure,
                     parameters=through[0]['config'],
                     parameters_by_objective={o: measured[o][0]['config'] for o in OBJECTIVES},
                     selected_candidate_ids={o: choices[o]['candidate_id'] for o in OBJECTIVES},
                     ranking_status={o: choices[o]['ranking_status'] for o in OBJECTIVES})
            for q in OBJECTIVES[1:]:
                g[q+'_ns'] = statistics.fmean(r['batch_average_write_ns'][q] for r in measured[q])
            for key in ('callback_to_producer_ratio', 'producer_seconds', 'callback_processing_seconds',
                        'wall_seconds', 'drain_seconds', 'failed_write_calls', 'backpressured_messages',
                        'producer_retry_seconds', 'cpu_cores', 'input_preparation_seconds'):
                g[key] = average(key)
            for key in ('actual_spsc_capacity', 'nominal_buffer_bytes', 'completed_rounds',
                        'effective_warmup_messages', 'prepared_input_bytes'):
                g[key if key != 'nominal_buffer_bytes' else 'nominal_buffer_mib'] = sorted(
                    {r[key]/1048576 if key == 'nominal_buffer_bytes' else r[key] for r in through + latency})
            for key in ('latency_samples', 'latency_tail_writes', 'timer'):
                g[key] = sorted({r[key] for r in latency})
            g['peak_rss_mib'] = max(r['peak_rss_mib'] for r in through + latency)
            g['latency_quantum_ns'] = max(r['latency_quantum_ns'] for r in latency)
            g['resolution_limited'] = any(r['resolution_limited'] for r in latency)
            g['p999_sample_count_warning'] = any(r['p999_sample_count_warning'] for r in latency)
            groups.append(g)
    if any(len(values) != 1 for values in callback_counts.values()):
        raise ValueError('callback loop counts differ across objectives or implementations')
    if callback_policy == 'zero' and any(next(iter(values)) != 0 for values in callback_counts.values()):
        raise ValueError('zero callback report contains business loops')
    calibration_path = directory/'callback-calibration.json'
    calibrations = json.loads(calibration_path.read_text()) if calibration_path.exists() else []
    if callback_policy == 'calibrated' and len(calibrations) != len(lengths):
        raise ValueError('callback calibration evidence is missing')
    identities = {key[0] for key in selected}
    validation = [r for r in all_rows if r['stage'] == 'validation' and r['candidate_id'] in identities]
    if {r['candidate_id'] for r in validation if r['result']['config']['verify_payload']} != identities:
        raise ValueError('a selected configuration lacks full payload validation')
    raw_rows = validation + list(selected.values())
    environment = dict(environment, timestamp=datetime.datetime.now().astimezone().isoformat(),
                       binary_sha256=environment['executable_sha256'],
                       timestamp_scope='report generation; measurements are preserved from the search directory')
    profile = dict(cache_mib=cache_mib, callback_policy=callback_policy, payload_bytes=lengths,
                   messages=messages, repetitions=repetitions, publish_count='按指标分别搜索')
    summary = dict(schema_version=2, profile=profile, environment=environment, groups=groups,
                   calibration=calibrations, search=dict(scope=best['scope'], settings=best['settings'],
                   groups=best['groups'], source_directory=str(directory.resolve())),
                   formal_runs=len(selected), validation_runs=len(validation),
                   calibration_runs=sum(r['stage'] == 'callback_calibration' for r in all_rows))
    return summary, raw_rows


def export_report(directory, output, html, cache_mib, callback_policy):
    directory, output = Path(directory), Path(output)
    summary, rows = assemble_report(directory, cache_mib, callback_policy)
    if output.exists():
        raise ValueError('output must not exist; previous measurements are preserved')
    output.mkdir(parents=True)
    for name in ('best.json', 'best.csv', 'candidates.csv', 'callback-calibration.json', 'environment.json'):
        source = directory/name
        if source.exists():
            shutil.copyfile(source, output/name)
    # Keep the entire search log separately from the selected formal/validation runs.
    shutil.copyfile(directory/'raw.jsonl', output/'search-raw.jsonl')
    (output/'raw.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
    for name, data in (('results.json', summary), ('profile.json', summary['profile']),
                       ('dependencies.json', summary['environment']['dependencies'])):
        (output/name).write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n')
    audit = dict(all_message_counts_and_fifo_verified=True, full_payload_checked_in_validation=True,
                 no_missing_runs=True, no_active_flush=True, one_unframed_two_buf_queue_only=True,
                 cache_budget_per_layer_mib=cache_mib, input_prepared_before_timing=True,
                 complete_variable_payload=True, independent_metric_parameters=True,
                 formal_runs=summary['formal_runs'], validation_runs=summary['validation_runs'],
                 binary_sha256=summary['environment']['binary_sha256'])
    (output/'audit.json').write_text(json.dumps(audit, indent=2)+'\n')
    write_html_report(summary, html, output)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('search_directory', type=Path)
    parser.add_argument('--cache-mib', type=int, required=True)
    parser.add_argument('--callback-policy', choices=('zero', 'calibrated'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--html', type=Path, required=True)
    args = parser.parse_args()
    if args.cache_mib < 1:
        parser.error('cache size must be positive')
    export_report(args.search_directory, args.output, args.html, args.cache_mib, args.callback_policy)
    print('Report:', args.html.resolve())


if __name__ == '__main__':
    main()
