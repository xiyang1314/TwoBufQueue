"""Verify empirical tails, independent-run averaging and complete report data."""
import sys
from pathlib import Path
import json
import math
import tempfile
import unittest
from html.parser import HTMLParser
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'benchmarks'))
import run_burst_showcase as pipeline
import burst_replay


class Contract(unittest.TestCase):
    def test_replay_restores_recorded_defaults(self):
        cmd=['program','--transport','rigtorp','--callback_iterations','16']
        recorded={'transport':'rigtorp','callback_iterations':64,
                  'spsc_idle_spins':4000,'rigtorp_write_prefetch':False,
                  'producer_qos':'user_interactive'}
        restored=burst_replay.append_queue_parameters(cmd,recorded)
        parsed=dict(zip(restored[1::2],restored[2::2]))
        self.assertEqual(parsed['--callback_iterations'],'64')
        self.assertEqual(parsed['--rigtorp_write_prefetch'],'false')
        self.assertEqual(parsed['--producer_qos'],'user_interactive')
        self.assertEqual(restored.count('--transport'),1)

    def test_record_compiler_and_optimization(self):
        with tempfile.TemporaryDirectory() as path:
            root=Path(path)
            (root/'CMakeCache.txt').write_text('CMAKE_CXX_COMPILER:FILEPATH=/test/clang++\nCMAKE_BUILD_TYPE:STRING=Release\nCMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\n')
            info=burst_replay.compiler_info(root/'program')
            self.assertEqual(info['CMAKE_CXX_COMPILER'],'/test/clang++')
            self.assertEqual(info['CMAKE_CXX_FLAGS_RELEASE'],'-O3 -DNDEBUG')
        with self.assertRaises(ValueError):
            burst_replay.choose_compiler(profile={'build':{'CMAKE_CXX_COMPILER':'/nonexistent/compiler'}})

    def test_nearest_rank_tails(self):
        samples=list(range(1,10001))
        self.assertEqual(pipeline.nearest(samples,.5),5000)
        self.assertEqual(pipeline.nearest(samples,.99),9900)
        self.assertEqual(pipeline.nearest(samples,.999),9990)
        with self.assertRaises(ValueError):pipeline.nearest([],.999)

    def fixture(self,folder):
        profile={'selected_business_kib':48,'bursts_mib':[1],'runs':3,
                 'final_window_repeats':2500,'samples_per_run':10000,
                 'callback_iterations':32,'process_timeout':900}
        manifest={'records':[]}
        (folder/'raw').mkdir()
        for label,transport,flags in pipeline.tuning.CASES:
            for run in range(1,4):
                samples=[float(v+(run-1)*100000) for v in range(1,10001)]
                points=[{'burst_bytes':1048576,'pre_business_ns':10.0+run,
                         'after_business_ns':samples[i:i+4],'setup_target_reached':True,
                         'pending_after':int(.8*256*1048576),'backpressured_messages':0}
                        for i in range(0,10000,4)]
                q={'callback_iterations':32,'source_block_size':4096,'source_pool_max':4,
                   'output_block_size':4096,'output_pool_max':65536}
                data={'verified':True,'accepted':81920000,'completed':81920000,
                      'formal':81920000,'business_bytes':49152,'queue_config':q,
                      'points':points,'query_group':16,'query_groups':4,
                      'transport':'baseline' if label=='baseline' else transport,
                      'qos_applied':True,'business_checksum':77}
                file=f'raw/48k_{label}_run{run}.json'
                (folder/file).write_text(json.dumps(data))
                manifest['records'].append({'file':file})
        pipeline.save(folder/'manifest.json',manifest)
        return profile

    def test_per_run_quantiles_then_mean(self):
        with tempfile.TemporaryDirectory() as directory:
            folder=Path(directory);profile=self.fixture(folder)
            result=pipeline.final_summary(folder,profile)
            for r in result['rows']:
                self.assertEqual(r['before_ns'],12.0)
                self.assertEqual(r['p50'],105000)
                self.assertEqual(r['p99'],109900)
                self.assertEqual(r['p999'],109990)
                self.assertEqual([x['samples'] for x in r['per_run']],[10000]*3)

    def test_insufficient_samples_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            folder=Path(directory);profile=self.fixture(folder);profile['samples_per_run']=10001
            with self.assertRaises(ValueError):pipeline.final_summary(folder,profile)

    def test_wrong_capacity_and_window_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            folder=Path(directory);self.fixture(folder)
            data=json.loads((folder/'raw/48k_TwoBufQueue_run1.json').read_text())
            data['queue_config']['source_pool_max']=5
            with self.assertRaises(ValueError):pipeline.validate(data,48,[1],2500,32)
            data['queue_config']['source_pool_max']=4
            data['points'][0]['after_business_ns'].pop()
            with self.assertRaises(ValueError):pipeline.validate(data,48,[1],2500,32)

    def test_one_table_standard_queues(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);folder=root/'formal';folder.mkdir();profile=self.fixture(folder)
            result=pipeline.final_summary(folder,profile)
            class Parser(HTMLParser):
                def __init__(self):super().__init__();self.tables=0;self.tb=0;self.text=[]
                def handle_starttag(self,t,attrs):
                    if t=='table':self.tables+=1
                    if t=='tr' and dict(attrs).get('class')=='tb':self.tb+=1
                def handle_data(self,d):self.text.append(d)
            report=root/'report.html';pipeline.render_report(root,report,result)
            parsed=Parser();parsed.feed(report.read_text())
            self.assertEqual(parsed.tables,1);self.assertEqual(parsed.tb,1)
            self.assertIn('耗时(ns/次)',''.join(parsed.text))
            self.assertIn('p50',''.join(parsed.text))
            self.assertNotIn('前耗时',''.join(parsed.text))
            self.assertNotIn('后耗时',''.join(parsed.text))
            self.assertNotIn('TwoBufQueue的优势',''.join(parsed.text))
            self.assertNotIn('ReaderWriterQueue · 成员复制',''.join(parsed.text))


if __name__=='__main__':unittest.main()
