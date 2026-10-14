#!/usr/bin/env python3
"""Contract checks use synthetic scores, never unstable performance thresholds."""
import importlib.util
from pathlib import Path
import random
import tempfile
import json
import unittest

spec = importlib.util.spec_from_file_location("tune_spsc", Path(__file__).resolve().parents[2]/"benchmarks/spsc/tune_spsc.py")
tune = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tune)


class SearchContract(unittest.TestCase):
    def test_completed_block_round_boundaries(self):
        c = dict(messages=6123, payload_bytes=32, source_block_size=65536,
                 warmup_messages=13, round_storage_bytes=32*2003,
                 is_scan_current=False, publish_count=2**32-1)
        r = dict(source_record_bytes=32, publication_alignment=1, effective_warmup_messages=13,
                 round_message_limit=2003, round_message_minimum=2003)
        self.assertEqual(tune.expected_round_count(c,r),4)
        self.assertEqual(tune.expected_round_count(dict(c,publish_count=1024),r),4)
        self.assertEqual(tune.expected_round_count(dict(c,is_scan_current=True),r),4)
        for changes in (dict(effective_warmup_messages=1024),dict(publication_alignment=1024),
                        dict(round_message_minimum=2002)):
            with self.assertRaises(ValueError):
                tune.expected_round_count(c,dict(r,**changes))

    def test_fixed_round_metadata_in_scanning_mode(self):
        for block in (1024,65536,1048576):
            for publish in (1,10):
                c=dict(messages=6123,payload_bytes=32,source_block_size=block,
                       warmup_messages=13,round_storage_bytes=32*2003,
                       is_scan_current=True,publish_count=publish,fixed_round_boundaries=True)
                r=dict(source_record_bytes=32,publication_alignment=1,effective_warmup_messages=13,
                       round_message_limit=2003,round_message_minimum=2003)
                self.assertEqual(tune.expected_round_count(c,r),4)
        with self.assertRaises(ValueError):
            tune.validate_space({"fixed_round_boundaries":[False,True]},
                                {"fixed_round_boundaries":False},"two_buf_queue")

    def test_count_ten_natural_round_boundaries(self):
        c = dict(messages=6123, payload_bytes=32, source_block_size=65536,
                 warmup_messages=13, round_storage_bytes=32*2003,
                 is_scan_current=True, publish_count=10)
        r = dict(source_record_bytes=32, publication_alignment=510,
                 effective_warmup_messages=510, round_message_limit=2003,
                 round_message_minimum=985)
        self.assertEqual(tune.expected_round_count(c,r),4)
        with self.assertRaises(ValueError):
            tune.expected_round_count(c,dict(r,publication_alignment=512))

    def test_fixed_workload_is_not_tunable(self):
        for field in ("messages", "payload_bytes", "callback_iterations", "warmup_messages", "producer_interval_ns", "latency_batch_size", "prepared_input", "variable_payload", "prepared_input_limit_bytes"):
            with self.assertRaises(ValueError):
                tune.validate_space({field: [1, 2]}, {field: 0}, "two_buf_queue")
        with self.assertRaises(ValueError):
            tune.validate_space({"publish_count": [1, 4]}, {"publish_count": 1}, "rigtorp")
        with self.assertRaises(ValueError):
            tune.validate_space({"block_bytes": [4096], "source_block_size": [4096]},
                                {"source_block_size": 4096}, "two_buf_queue")

    def test_rigtorp_prefetch_is_queue_specific(self):
        defaults={"rigtorp_write_prefetch":True}
        tune.validate_space({"rigtorp_write_prefetch":[False,True]},defaults,"rigtorp")
        tune.validate_length_reference({"rigtorp_write_prefetch":False},defaults)
        for transport in ("two_buf_queue","readerwriterqueue","atomic_queue"):
            with self.assertRaises(ValueError):
                tune.validate_space({"rigtorp_write_prefetch":[False,True]},defaults,transport)

    def test_readerwriterqueue_copy_policy_is_queue_specific(self):
        defaults={"readerwriterqueue_copy_after_zero":False}
        tune.validate_space({"readerwriterqueue_copy_after_zero":[False,True]},defaults,"readerwriterqueue")
        tune.validate_length_reference({"readerwriterqueue_copy_after_zero":True},defaults)
        for transport in ("two_buf_queue","rigtorp","atomic_queue"):
            with self.assertRaises(ValueError):
                tune.validate_space({"readerwriterqueue_copy_after_zero":[False,True]},defaults,transport)

    def test_coupled_capacity_alias(self):
        result = tune.effective_config({"payload_bytes": 8, "messages": 100},
                                       {"block_bytes": 4096, "source_pool_blocks": 2})
        self.assertEqual(result["source_block_size"], result["output_block_size"])
        self.assertEqual(result["source_pool_min"], result["source_pool_max"])
        self.assertEqual(result["payload_bytes"], 8)
        self.assertEqual(result["messages"], 100)

    def test_cache_alias_preserves_bytes(self):
        base=dict(source_block_size=65536,output_block_size=65536,source_pool_min=128,
                  source_pool_max=128,output_pool_min=128,output_pool_max=128,
                  source_queue_capacity=256,output_queue_capacity=256)
        for block in (4096,16384,262144,1048576):
            result=tune.effective_config(base,{"cache_block_bytes":block})
            for prefix in ("source","output"):
                self.assertEqual(result[prefix+"_block_size"]*result[prefix+"_pool_max"],8*1048576)
                self.assertEqual(result[prefix+"_pool_min"],result[prefix+"_pool_max"])
                self.assertEqual(result[prefix+"_queue_capacity"],2*result[prefix+"_pool_max"])
        with self.assertRaises(ValueError):
            tune.effective_config(base,{"cache_block_bytes":12345})
        with self.assertRaises(ValueError):
            tune.validate_space({"cache_block_bytes":[4096],"source_pool_max":[128]},base,"two_buf_queue")

    def test_per_length_native_capacity(self):
        defaults = dict(source_block_size=65536, output_block_size=65536,
                        spsc_capacity=0, spsc_idle_spins=1000)
        tune.validate_length_reference(dict(block_bytes=4194304, spsc_capacity=261121), defaults)
        for changes in (dict(spsc_unknown=1), dict(messages=1),
                        dict(block_bytes=1024, source_block_size=1024)):
            with self.assertRaises(ValueError):
                tune.validate_length_reference(changes, defaults)

    def test_per_length_reference_preserves_common_budget(self):
        search=tune.ParameterSearch.__new__(tune.ParameterSearch)
        search.base=dict(source_block_size=65536,output_block_size=65536,source_pool_min=128,
                  source_pool_max=128,output_pool_min=128,output_pool_max=128,
                  source_queue_capacity=512,output_queue_capacity=512,publish_count=4)
        search.base_by_payload={"8":{"cache_block_bytes":1048576,"publish_count":32},
                                "64":{"publish_count":1024}}
        self.assertEqual(search.length_base(8)["source_pool_max"],8)
        self.assertEqual(search.length_base(8)["publish_count"],32)
        self.assertEqual(search.length_base(64)["publish_count"],1024)
        self.assertEqual(search.length_base(16),search.base)

    def test_grid_and_budget(self):
        base = {"payload_bytes": 8, "publish_count": 1, "reader_spin_count": 0}
        space = {"publish_count": [1, 4], "reader_spin_count": [0, 32, 256]}
        all_values = tune.initial_candidates(base, space, "grid", 0, random.Random(42))
        self.assertEqual(len(all_values), 6)
        limited = tune.initial_candidates(base, space, "grid", 3, random.Random(42))
        self.assertLessEqual(len(limited), 3)
        self.assertEqual(limited[0], base)
        self.assertEqual(limited, tune.initial_candidates(base, space, "grid", 3, random.Random(42)))
        self.assertEqual(tune.initial_candidates(base, space, "coordinate", 1, random.Random(42)), [base])

    def test_independent_metric_winners(self):
        candidates = []
        for i in range(4):
            metrics = dict(throughput=1, p50=10, p99=10, p999=10)
            metrics[tune.OBJECTIVES[i]] = 100 if i == 0 else 0
            candidates.append(dict(candidate_id=str(i), metrics=metrics))
        for i, objective in enumerate(tune.OBJECTIVES):
            self.assertEqual(tune.select_best(candidates, objective)["candidate_id"], str(i))
        with self.assertRaises(ValueError):
            tune.select_best([], "throughput")

    def test_clocked_throughput_not_used_for_ranking(self):
        def row(mode, throughput, p50=2):
            return {"result": dict(config={"mode": mode}, m_messages_per_second=throughput,
                cpu_cores=1, peak_rss_mib=2, failed_write_calls=0, actual_spsc_capacity=16,
                nominal_buffer_bytes=128, latency_samples=1000, timer_quantum_ns=1,
                timer_overhead_ns={"p50": 1}, resolution_limited=True, p999_sample_count_warning=True,
                successful_write_ns={"p50": p50, "p99": 4, "p999": 6})}
        result = tune.aggregate([row("throughput", 10), row("throughput", 20), row("throughput", 90),
                                 row("write_latency", 999, 2), row("write_latency", 888, 4), row("write_latency", 777, 15)])
        self.assertEqual(result["throughput"], 40)
        self.assertEqual(result["p50"], 7)
        self.assertIsNone(tune.aggregate([row("throughput", 10)]))

    def test_payloads_have_separate_candidate_identity(self):
        a = dict(payload_bytes=8, callback_iterations=0, messages=100, mode="throughput", random_seed=1)
        b = dict(a, messages=1000, mode="write_latency", random_seed=2)
        self.assertEqual(tune.candidate_id(a), tune.candidate_id(b))
        self.assertNotEqual(tune.candidate_id(a), tune.candidate_id(dict(a, payload_bytes=16)))
        self.assertNotEqual(tune.candidate_id(a), tune.candidate_id(dict(a, callback_iterations=1)))

    def test_incomplete_candidate_is_excluded(self):
        search = tune.ParameterSearch.__new__(tune.ParameterSearch)
        search.settings = dict(final_messages=1000, final_repetitions=2)
        search.rows = []
        search.rng = random.Random(42)
        # Simulate one failed latency trial among otherwise successful measurements.
        def run_one(config, stage, trial, identity):
            if config["mode"] == "write_latency" and trial == 1:
                return None
            search.rows.append(dict(candidate_id=identity, stage=stage, result={"config": config}))
        search.run_one = run_one
        result = search.evaluate([dict(payload_bytes=8, random_seed=42)], "final")
        self.assertEqual(result, [])

    def test_group_quantiles_use_scaled_timer_grain(self):
        def row(mode):
            return {"result": dict(config={"mode": mode, "latency_batch_size": 1000},
                latency_kind="batch_average", m_messages_per_second=100, cpu_cores=2, peak_rss_mib=2,
                failed_write_calls=0, actual_spsc_capacity=1024, nominal_buffer_bytes=8192,
                latency_samples=1000, timer_quantum_ns=41.667, latency_quantum_ns=0.041667,
                timer_overhead_ns={"p50": 0}, resolution_limited=False, p999_sample_count_warning=True,
                successful_write_ns=None, batch_average_write_ns={"p50": 1.5, "p99": 4, "p999": 6})}
        rows = [row("throughput"), row("write_latency")]
        metrics = tune.aggregate(rows)
        self.assertEqual(metrics["p50"], 1.5)
        self.assertEqual(metrics["p50_status"], "observed")
        self.assertEqual(metrics["latency_kind"], "batch_average")
        rows.append(row("write_latency"))
        rows[-1]["result"]["config"]["latency_batch_size"] = 2000
        with self.assertRaises(ValueError):
            tune.aggregate(rows)

    def test_backpressure_means_and_unavailable_native_stats(self):
        def row(mode, failed, messages, pressured, seconds, internal):
            return {"result": dict(config={"mode": mode, "messages": messages, "latency_batch_size": 1000},
                latency_kind="batch_average", m_messages_per_second=100, cpu_cores=2, peak_rss_mib=2,
                failed_write_calls=failed, actual_spsc_capacity=32, nominal_buffer_bytes=256,
                latency_samples=10000, timer_quantum_ns=41.667, latency_quantum_ns=0.041667,
                timer_overhead_ns={"p50": 0}, resolution_limited=False, p999_sample_count_warning=True,
                successful_write_ns=None, batch_average_write_ns={"p50": 1.5, "p99": 4, "p999": 6},
                backpressured_messages=pressured, producer_retry_seconds=seconds,
                producer_retry_max_seconds=seconds/2, library_backpressure=internal)}
        pressure={k: 2 for k in ("writer_pressure_events","writer_rejected_attempts","writer_blocked_events",
                                "writer_blocked_ticks","copy_memory_full_events","copy_backpressured_frames")}
        rows=[row("throughput",2,100,1,0.2,pressure),row("throughput",6,100,2,0.6,pressure),
              row("write_latency",999,100,99,100,pressure)]
        m=tune.aggregate(rows)
        self.assertEqual(m["failed_write_calls"],4)
        self.assertEqual(m["backpressured_messages"],1.5)
        self.assertAlmostEqual(m["producer_retry_seconds"],0.4)
        self.assertAlmostEqual(m["backpressured_message_fraction"],0.015)
        self.assertAlmostEqual(m["failed_attempt_fraction"],(2/102+6/106)/2)
        self.assertEqual(m["library_backpressure"]["copy_memory_full_events"],2)
        for r in rows: r["result"]["library_backpressure"]=None
        self.assertIsNone(tune.aggregate(rows)["library_backpressure"])

    def test_round_count_scales_equally_by_payload(self):
        search = tune.ParameterSearch.__new__(tune.ParameterSearch)
        search.settings = dict(screen_messages=100, final_messages=1000, screen_min_rounds=2, final_min_rounds=3)
        for transport in tune.TRANSPORTS:
            config = dict(payload_bytes=8, round_storage_bytes=10000, transport=transport)
            self.assertEqual(search.measurement_count(config, "screen"), 2500)
            self.assertEqual(search.measurement_count(config, "final"), 3750)
        self.assertEqual(search.measurement_count(dict(payload_bytes=64, round_storage_bytes=10000), "final"), 1000)

    def test_natural_publication_rounds(self):
        config = dict(payload_bytes=64, source_block_size=4096, publish_count=32,
                      warmup_messages=0, round_storage_bytes=64*2003, messages=6123)
        result = dict(source_record_bytes=64, publication_alignment=32, effective_warmup_messages=32,
                      round_message_limit=2003, round_message_minimum=1941)
        self.assertEqual(tune.expected_round_count(config, result), 4)
        # A 500-record warmup crosses a 511-record block to the next 32-record publication.
        x = dict(config, payload_bytes=8, source_block_size=4088, warmup_messages=500,
                 round_storage_bytes=8*2003)
        y = dict(result, source_record_bytes=8, effective_warmup_messages=543)
        self.assertEqual(tune.expected_round_count(x, y), 4)
        for changes in (dict(effective_warmup_messages=512), dict(publication_alignment=3),
                        dict(round_message_limit=2004), dict(source_record_bytes=12)):
            with self.assertRaises(ValueError):
                tune.expected_round_count(x, dict(y, **changes))

    def test_callback_loop_calibration(self):
        settings = dict(target_ratio=2.5, min_ratio=2, max_ratio=3, repetitions=5,
                        messages=100, min_rounds=2, max_iterations=1024,
                        max_candidates=10, reference_publish_count=16)
        tune.validate_calibration(settings)
        tune.validate_calibration(dict(settings,min_rounds=0))
        calls = []
        def probe(n):
            calls.append(n)
            return 0.4*n
        self.assertEqual(tune.choose_callback_iterations(probe, settings), 6)
        self.assertEqual(len(calls), len(set(calls)))
        self.assertGreaterEqual(min(calls), 1)
        self.assertEqual(tune.choose_callback_iterations(lambda n: 4*n, settings), 1)
        bounded = dict(settings, max_iterations=3)
        self.assertEqual(tune.choose_callback_iterations(lambda n: 0.1*n, bounded), 3)
        for changes in ({"min_ratio": 4}, {"target_ratio": float("nan")},
                        {"repetitions": 0}, {"min_rounds": -1}, {"reference_publish_count": 3}, {"max_iterations": 2**32}):
            with self.assertRaises(ValueError):
                tune.validate_calibration(dict(settings, **changes))
        with self.assertRaises(ValueError):
            tune.choose_callback_iterations(lambda n: float("nan"), settings)

    def test_zero_is_not_a_latency_winner(self):
        candidates = []
        for identity, value, speed in (("zero", 0, 10), ("one_tick", 41.667, 20)):
            metrics = dict(throughput=speed, p50=value, p99=41.667, p999=83.334,
                           timer_quantum_ns=41.667, latency_quantum_ns=41.667,
                           timer_overhead_p50_ns=0, resolution_limited=True,
                           p999_sample_count_warning=False, cpu_cores=2)
            candidates.append(dict(candidate_id=identity, metrics=metrics, stage="final",
                                   parameters={"transport": "two_buf_queue"}))
        result = tune.objective_result(candidates, "p50", 8, 0)
        self.assertEqual(result["ranking_status"], "unresolved")
        self.assertIsNone(result["objective_value"])
        self.assertEqual(result["selection_basis"], "throughput_representative")
        self.assertEqual(result["candidate_id"], "one_tick")
        search = tune.ParameterSearch.__new__(tune.ParameterSearch)
        search.settings = dict(screen_repetitions=2, final_repetitions=3)
        search.failures = []
        with tempfile.TemporaryDirectory() as directory:
            search.output = Path(directory)
            results = [tune.objective_result(candidates, objective, 8, 0) for objective in tune.OBJECTIVES]
            search.write_reports(candidates, [], results)
            saved = json.loads((search.output/"best.json").read_text())
            self.assertEqual([v["objective"] for v in saved["winners"]], ["throughput"])
            self.assertEqual(len(saved["unresolved"]), 3)
            self.assertIn("精度不足", (search.output/"SUMMARY.md").read_text())


if __name__ == "__main__":
    unittest.main()
