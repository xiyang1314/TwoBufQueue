"""Choose one measured configuration per transport/mode for all four lengths.

This is offline analysis: no benchmark execution or changes to source data.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import statistics

WEIGHTS = {'throughput': .35, 'p50': .30, 'p99': .20, 'p999': .15}
CV_LIMITS = {'throughput': .20, 'p50': .25, 'p99': .40, 'p999': .60}
LENGTHS = (8, 16, 32, 64)
TB_KEYS = ('source_block_size', 'output_block_size', 'output_frame_capacity',
           'reader_spin_count', 'rescan_interval_ns', 'rescan_max_interval_ns')
NATIVE_KEYS = ('spsc_idle_spins', 'spsc_idle_sleep_ns', 'retry_yield_every')

def mode_of(p):
    return 'session' if p['use_write_session'] else 'publish1' if p['publish_count'] == 1 else 'publish_max'

def key_of(p):
    return tuple(p[k] for k in (TB_KEYS if p['transport'] == 'two_buf_queue' else NATIVE_KEYS))

def analyze(data, destination, extra=None, reference_file=None):
    candidates = json.loads((data / 'all_candidates.json').read_text())
    # The original grid is the only stage containing full, equal four-length
    # coverage. All candidates have 3 throughput and 3 latency runs plus a
    # separate complete-payload/FIFO validation. Never mix screening and final.
    candidates = [c for c in candidates if c['stage'] == 'screen']
    if extra:
        candidates.extend(json.loads((extra / 'candidates.json').read_text()))
    rows = defaultdict(list)
    validation_ids = set()
    for raw_file in ([data / 'raw.jsonl'] + ([extra / 'raw.jsonl'] if extra else [])):
        with raw_file.open() as f:
            for line in f:
                r = json.loads(line)
                if r['stage'] == 'screen':
                    rows[r['candidate_id']].append(r)
                elif r['stage'] == 'validation':
                    validation_ids.add(r['candidate_id'])
    frozen = json.loads(reference_file.read_text()) if reference_file else None
    valid = {}
    for c in candidates:
        rs = rows[c['candidate_id']]
        okay = len(rs) == 6 and c['candidate_id'] in validation_ids
        if okay:
            okay = not any(r['result']['failed_write_calls'] or
                           (r['result'].get('library_backpressure') or {}).get('writer_pressure_events', 0)
                           for r in rs)
        cvs = {}
        for o in WEIGHTS:
            relevant = [r['result'] for r in rs if r['result']['config']['mode'] ==
                        ('throughput' if o == 'throughput' else 'write_latency')]
            vals = [r['m_messages_per_second'] if o == 'throughput' else r['batch_average_write_ns'][o]
                    for r in relevant]
            mean = statistics.mean(vals) if vals else 0
            cvs[o] = statistics.pstdev(vals) / mean if mean > 0 else float('inf')
            if len(vals) != 3 or mean <= 0:
                okay = False
            if o != 'throughput' and any(r['resolution_limited'] for r in relevant):
                okay = False
        valid[c['candidate_id']] = {'valid': okay, 'cv': cvs}
    output = {'policy': {'weights': WEIGHTS, 'payload_weights': {str(n): .25 for n in LENGTHS},
                         'normalization': 'min(1, throughput / reference_throughput); min(1, reference_latency / latency)',
                         'reference': 'per-transport, per-mode, per-length best metric among validated screening configurations meeting that metric CV limit',
                         'score': '100 * mean_over_four_lengths(sum(weight * normalized_metric))',
                         'plateau': 'three adjacent common configurations; pick center; <=10% four-length aggregate score spread',
                         'ranking': 'maximize minimum four-length total score of three adjacent configurations, then center score',
                         'reference_metric_max_cv': CV_LIMITS, 'weighted_four_length_trial_score_max_cv': .20,
                         'stage': 'screen', 'repetitions': 3,
                         'same_configuration_for_all_metrics': True,
                         'same_tunable_configuration_for_all_lengths': True,
                         'additional_measurement': bool(extra),
                         'reference_frozen_before_additional_measurement': bool(frozen)}, 'groups': [], 'missing_groups': []}
    for transport in ('two_buf_queue', 'readerwriterqueue', 'rigtorp', 'atomic_queue'):
        for mode in (('session', 'publish_max', 'publish1') if transport == 'two_buf_queue' else ('native',)):
            cs = [c for c in candidates if c['parameters']['transport'] == transport and
                  (transport != 'two_buf_queue' or mode_of(c['parameters']) == mode)]
            good = [c for c in cs if valid[c['candidate_id']]['valid']]
            refs = {}
            for n in LENGTHS:
                gg = [c for c in good if c['parameters']['payload_bytes'] == n]
                if not gg:
                    raise ValueError(f'No CV-qualified result: {transport}, {mode}, {n}')
                refs[str(n)] = {o: (max if o == 'throughput' else min)(c['metrics'][o] for c in gg
                                if valid[c['candidate_id']]['cv'][o] <= CV_LIMITS[o]) for o in WEIGHTS}
            if frozen:
                refs = next(g['references'] for g in frozen['groups'] if g['transport']==transport and g['writer_mode']==mode)
            joint = defaultdict(dict)
            for c in good:
                joint[key_of(c['parameters'])][c['parameters']['payload_bytes']] = c
            joint = {k:v for k,v in joint.items() if set(v) == set(LENGTHS)}
            points = []
            for key, lengths in joint.items():
                per_length = {}
                for n,c in lengths.items():
                    ref = refs[str(n)]
                    normalized = {o: min(1,c['metrics'][o]/ref[o] if o=='throughput' else ref[o]/c['metrics'][o]) for o in WEIGHTS}
                    rs=rows[c['candidate_id']]
                    tr=sorted((r for r in rs if r['result']['config']['mode']=='throughput'),key=lambda r:r['trial'])
                    lr=sorted((r for r in rs if r['result']['config']['mode']=='write_latency'),key=lambda r:r['trial'])
                    trial_scores=[100*sum(WEIGHTS[o]*min(1,t['result']['m_messages_per_second']/ref[o] if o=='throughput'
                                                       else ref[o]/l['result']['batch_average_write_ns'][o]) for o in WEIGHTS)
                                  for t,l in zip(tr,lr)]
                    per_length[str(n)] = {'candidate':c, 'normalized':normalized,
                                          'score':100*sum(WEIGHTS[o]*normalized[o] for o in WEIGHTS),
                                          'cv':valid[c['candidate_id']]['cv'], 'trial_scores':trial_scores,
                                          'weighted_trial_score_cv':statistics.pstdev(trial_scores)/statistics.mean(trial_scores)}
                total_trial_scores=[statistics.mean(v['trial_scores'][i] for v in per_length.values()) for i in range(3)]
                total_cv=statistics.pstdev(total_trial_scores)/statistics.mean(total_trial_scores)
                if total_cv > .20:
                    continue
                points.append({'common_parameters':dict(zip(TB_KEYS if transport=='two_buf_queue' else NATIVE_KEYS,key)),
                               'lengths':per_length,
                               'four_length_trial_scores':total_trial_scores,
                               'weighted_four_length_trial_score_cv':total_cv,
                               'score':statistics.mean(v['score'] for v in per_length.values())})
            # Neighborhood membership uses all actually measured common points,
            # not only CV-qualified ones, so invalid points cannot be skipped.
            measured = defaultdict(set)
            for c in cs:
                measured[key_of(c['parameters'])].add(c['parameters']['payload_bytes'])
            measured_keys = {k for k,v in measured.items() if set(v)==set(LENGTHS)}
            by_key = {tuple(p['common_parameters'].values()):p for p in points}
            options = []
            axes = (('source_block_size',0),) if transport=='two_buf_queue' else (('spsc_idle_spins',0),('spsc_idle_sleep_ns',1))
            for axis,index in axes:
                families = defaultdict(list)
                for key in measured_keys:
                    families[key[:index]+key[index+1:]].append(key)
                for family in families.values():
                    family.sort(key=lambda k:k[index])
                    for i in range(1,len(family)-1):
                        keys=family[i-1:i+2]
                        if any(k not in by_key for k in keys):
                            continue
                        xs=[k[index] for k in keys]
                        if transport=='two_buf_queue' and (xs[1]!=xs[0]*2 or xs[2]!=xs[1]*2):
                            continue
                        window=[by_key[k] for k in keys]
                        spreads={str(n):max(p['lengths'][str(n)]['score'] for p in window)/
                                 min(p['lengths'][str(n)]['score'] for p in window)-1 for n in LENGTHS}
                        total_spread=max(p['score'] for p in window)/min(p['score'] for p in window)-1
                        if total_spread > .10:
                            continue
                        options.append({'axis':axis,'points':xs,'center':window[1],
                                        'neighbor_scores':[p['score'] for p in window],
                                        'neighbor_candidates':{str(n):[p['lengths'][str(n)]['candidate']['candidate_id'] for p in window] for n in LENGTHS},
                                        'per_length_relative_score_spread':spreads,
                                        'total_relative_score_spread':total_spread,
                                        'conservative_score':min(p['score'] for p in window)})
            options.sort(key=lambda o:(o['conservative_score'],o['center']['score']),reverse=True)
            group={'transport':transport,'writer_mode':mode,'references':refs,'common_candidates':points,
                   'qualified_plateaus':len(options),'top_plateaus':options[:10],
                   'selected':options[0] if options else None}
            output['groups'].append(group)
            if not options:
                output['missing_groups'].append({'transport':transport,'writer_mode':mode,
                                                 'cv_qualified_common_configurations':len(points)})
    output['source_sha256']={f:hashlib.sha256((data/f).read_bytes()).hexdigest() for f in
                            ('all_candidates.json','raw.jsonl','comparison_results.json','audit.json','environment.json')}
    if extra:
        output['additional_measurement_sha256']={f:hashlib.sha256((extra/f).read_bytes()).hexdigest()
                                                for f in ('candidates.json','raw.jsonl','audit.json')}
    if reference_file:
        output['normalization_reference_sha256']=hashlib.sha256(reference_file.read_bytes()).hexdigest()
    destination.write_text(json.dumps(output,ensure_ascii=False,indent=2)+'\n')
    for group in output['groups']:
        print(group['transport'],group['writer_mode'],'common qualified',len(group['common_candidates']),
              'plateaus',group['qualified_plateaus'])
        if group['selected']:
            o=group['selected'];print(' CHOSEN',o['center']['common_parameters'],'score',round(o['center']['score'],3),
                                      'conservative',round(o['conservative_score'],3),'neighbors',o['points'])
            for n,v in o['center']['lengths'].items():
                print(' ',n,'id',v['candidate']['candidate_id'],'score',round(v['score'],3),
                      'metrics',{k:round(v['candidate']['metrics'][k],3) for k in WEIGHTS})
    print('Missing',output['missing_groups'])

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--extra',type=Path);p.add_argument('--reference',type=Path)
    a=p.parse_args();analyze(a.data,a.output,a.extra,a.reference)
