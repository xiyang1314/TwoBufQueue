"""Check that report columns preserve independent metric selections and exact counts."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'benchmarks/spsc'))
from export_spsc_report import assemble_report


class ReportContract(unittest.TestCase):
    def fixture(self, directory):
        rows, winners = [], []
        for transport in ('two_buf_queue', 'readerwriterqueue', 'rigtorp', 'atomic_queue'):
            for index, objective in enumerate(('throughput', 'p50', 'p99', 'p999')):
                identity = transport + '-' + objective
                mode = 'throughput' if objective == 'throughput' else 'write_latency'
                c = dict(payload_bytes=8, messages=10000, callback_iterations=0, mode=mode,
                         transport=transport, prepared_input=True, variable_payload=True,
                         source_block_size=65536, output_block_size=65536,
                         source_pool_min=16, source_pool_max=16, output_pool_min=16, output_pool_max=16,
                         spsc_capacity=0, spsc_buffer_bytes=1048576, output_mode='spsc',
                         producer_qos='unchanged', producer_cpu=-1, latency_batch_size=1000,
                         publish_count=2**index, verify_payload=False)
                r = dict(config=c, consumed=10000, timed_callback_messages=10000, seconds=.01,
                         producer_seconds=.01, timing_scope='producer_rounds', source_record_bytes=8,
                         m_messages_per_second=1+index, payload_gb_per_second=.1,
                         library_backpressure=(dict(scope='formal_drain',copy_memory_full_events=2,
                                                    writer_blocked_counter_kind='steady_clock_ticks')
                                               if transport=='two_buf_queue' else None),
                         callback_to_producer_ratio=2, callback_processing_seconds=.02, wall_seconds=.03,
                         drain_seconds=.01, failed_write_calls=0, backpressured_messages=0,
                         producer_retry_seconds=0, cpu_cores=1, input_preparation_seconds=.001,
                         actual_spsc_capacity=131072, nominal_buffer_bytes=1048576,
                         completed_rounds=2, effective_warmup_messages=1024, prepared_input_bytes=80000,
                         latency_samples=10, latency_tail_writes=0, timer='steady', peak_rss_mib=3,
                         latency_quantum_ns=.01, resolution_limited=False, p999_sample_count_warning=True,
                         batch_average_write_ns=dict(p50=10+index, p99=20+index, p999=30+index))
                winners.append(dict(candidate_id=identity, payload_bytes=8, parameters=c,
                                    objective=objective, ranking_status='observed'))
                for trial in range(3):
                    rows.append(dict(stage='final', candidate_id=identity, trial=trial, result=r))
                checked=dict(r,config=dict(c,verify_payload=True))
                rows.append(dict(stage='validation', candidate_id=identity, trial=0, result=checked))
        best=dict(scope='declared fixture', winners=winners, unresolved=[], failed_runs=0,
                  settings=dict(final_repetitions=3,final_messages=10000),groups=[])
        environment=dict(build=dict(optimized=True,sanitized=False),profile=dict(payload_bytes=[8]),
                         executable_sha256='0'*64,dependencies={})
        for name, value in [('best.json',best),('environment.json',environment)]:
            (directory/name).write_text(json.dumps(value))
        self.save_rows(directory,rows)
        return rows

    def save_rows(self, directory, rows):
        (directory/'raw.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))

    def test_independent_metric_columns(self):
        with tempfile.TemporaryDirectory() as path:
            directory=Path(path);self.fixture(directory)
            report,rows=assemble_report(directory,1,'zero')
            self.assertEqual(report['formal_runs'],48)
            self.assertEqual(report['validation_runs'],16)
            for g in report['groups']:
                self.assertEqual((g['p50_ns'],g['p99_ns'],g['p999_ns']),(11,22,33))
                self.assertEqual(g['parameters_by_objective']['p999']['publish_count'],8)
                self.assertEqual(g['parameters']['publish_count'],1)
                if g['transport']=='two_buf_queue':
                    self.assertEqual(g['internal_backpressure']['scope'],'formal_drain')
                    self.assertEqual(g['internal_backpressure']['copy_memory_full_events'],2)

    def test_missing_repetition_rejected(self):
        with tempfile.TemporaryDirectory() as path:
            directory=Path(path);rows=self.fixture(directory);self.save_rows(directory,rows[1:])
            with self.assertRaises(ValueError):assemble_report(directory,1,'zero')

    def test_changed_cache_and_input_rejected(self):
        for change in ({'source_pool_max':17},{'prepared_input':False},{'variable_payload':False}):
            with tempfile.TemporaryDirectory() as path:
                directory=Path(path);rows=self.fixture(directory)
                rows[0]['result']['config'].update(change);self.save_rows(directory,rows)
                with self.assertRaises(ValueError):assemble_report(directory,1,'zero')


if __name__=='__main__':unittest.main()
