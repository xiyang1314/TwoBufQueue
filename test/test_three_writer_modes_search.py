#!/usr/bin/env python3
"""Check fixed reader/round invariants of the reusable three-mode search."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"benchmarks"))
import tune_three_writer_modes as search

class FixedBudgetContract(unittest.TestCase):
    def setUp(self):
        self.base=dict(fixed_round_boundaries=True,round_storage_bytes=2*search.GIB*4//5,
                       prepared_input=True,variable_payload=True,callback_iterations=0,
                       latency_batch_size=1000)
    def test_every_mode_length_and_writer_block_keeps_reader_and_round(self):
        configs=search.blocks_candidates(self.base,[8,16,32,64],2*search.GIB)
        self.assertEqual(len(configs),252)
        self.assertEqual({c["round_storage_bytes"] for c in configs},{1717986918})
        self.assertEqual({c["output_block_size"]*c["output_pool_max"] for c in configs},{2*search.GIB})
        for mode,settings in search.MODES.items():
            matching=[c for c in configs if search.mode_of(c)==mode]
            self.assertEqual(len(matching),84)
            self.assertEqual({c["source_block_size"] for c in matching},{1<<n for n in range(10,31)})
            for c in matching:
                search.assert_invariants(c,2*search.GIB,1717986918)
    def test_ordinary_only_grid_never_contains_sessions(self):
        modes=("publish1","publish_max")
        configs=search.blocks_candidates(self.base,[8,16,32,64],2*search.GIB,modes)
        self.assertEqual(len(configs),168)
        self.assertEqual({search.mode_of(c) for c in configs},set(modes))
        self.assertFalse(any(c["use_write_session"] for c in configs))
        for c in configs:
            search.assert_invariants(c,2*search.GIB,1717986918)

    def test_pressure_constraint_uses_every_throughput_and_latency_run(self):
        c=dict(candidate_id="x",stage="final")
        rows=[dict(candidate_id="x",stage="final",result=dict(failed_write_calls=0,
              library_backpressure=dict(writer_pressure_events=0,copy_memory_full_events=12))) for _ in range(6)]
        self.assertTrue(search.pressure_free(c,rows))
        rows[-1]["result"]["library_backpressure"]["writer_pressure_events"]=1
        self.assertFalse(search.pressure_free(c,rows))
        rows[-1]["result"]["library_backpressure"]["writer_pressure_events"]=0
        rows[-1]["result"]["failed_write_calls"]=1
        self.assertFalse(search.pressure_free(c,rows))
        self.assertFalse(search.pressure_free(c,[]))

    def test_reader_capacity_must_be_exact_without_truncated_block_count(self):
        with self.assertRaises(ValueError):
            search.with_pools(self.base,65536,8*search.MIB,10*65536,2*search.GIB)
        with self.assertRaises(ValueError):
            search.with_pools(self.base,65536,8*search.MIB,32768,2*search.GIB)
    def test_writer_needs_two_blocks_and_round_cannot_change(self):
        with self.assertRaises(ValueError):
            search.with_pools(self.base,65536,65536,65536,2*search.GIB)
        c=search.blocks_candidates(self.base,[32],2*search.GIB)[0]
        with self.assertRaises(AssertionError):
            search.assert_invariants(dict(c,round_storage_bytes=12345),2*search.GIB,1717986918)

if __name__=="__main__":
    unittest.main()
