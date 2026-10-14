"""Plot saved matched-parameter write costs with ReportLab; never select trials or rerun."""
import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET

import reportlab
from reportlab.graphics.charts.barcharts import VerticalBarChart
from reportlab.graphics.shapes import Drawing, Rect, String
from reportlab.graphics import renderSVG
from reportlab.lib.colors import HexColor

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--data', type=Path, default=ROOT/'docs/performance_data/release_20261011_rigtorp_fixed')
parser.add_argument('--output', type=Path, default=ROOT/'docs/assets/write_cost_8m.svg')
args = parser.parse_args()
series = []
repetitions = None
for mode,label,color in [('normal','Ordinary write','#3864d9'),('session','WriteSession','#087f73')]:
    result = json.loads((args.data/f'8m_{mode}/results.json').read_text(encoding='utf-8'))
    assert result['profile']['cache_mib'] == 8
    rows = sorted((g for g in result['groups'] if g['transport']=='two_buf_queue'),key=lambda g:g['payload_bytes'])
    assert [g['payload_bytes'] for g in rows] == [8,16,32,64]
    runs = result['profile']['repetitions']
    if repetitions is None: repetitions = runs
    assert repetitions == runs
    assert all(g['callback_iterations']==0 and len(g['p50_runs_ns'])==runs for g in rows)
    series.append(dict(mode=mode,label=label,color=color,rows=rows))
for a,b in zip(series[0]['rows'],series[1]['rows']):
    pa,pb=dict(a['parameters']),dict(b['parameters'])
    pa.pop('use_write_session');pb.pop('use_write_session')
    assert pa==pb

drawing=Drawing(820,370)
drawing.add(Rect(0,0,820,370,fillColor=HexColor('#f7f9fc'),strokeColor=None))
drawing.add(String(30,341,'TwoBufQueue: ordinary write vs WriteSession',fontName='Helvetica-Bold',fontSize=18,fillColor=HexColor('#172b4d')))
drawing.add(String(30,321,f'p50 of 1,000-write group averages | mean of {repetitions} measured runs',fontName='Helvetica',fontSize=11,fillColor=HexColor('#475569')))
chart=VerticalBarChart()
chart.x=80;chart.y=63;chart.width=670;chart.height=224
chart.data=[[g['p50_ns'] for g in s['rows']] for s in series]
chart.categoryAxis.categoryNames=['8 B','16 B','32 B','64 B']
chart.categoryAxis.labels.fontName='Helvetica';chart.categoryAxis.labels.fontSize=11
chart.categoryAxis.strokeColor=HexColor('#94a3b8')
chart.valueAxis.valueMin=0;chart.valueAxis.valueMax=3;chart.valueAxis.valueStep=.5
chart.valueAxis.labels.fontName='Helvetica';chart.valueAxis.labels.fontSize=10
chart.valueAxis.visibleGrid=True;chart.valueAxis.gridStrokeColor=HexColor('#dce3ed')
chart.valueAxis.strokeColor=HexColor('#94a3b8')
chart.barWidth=16;chart.barSpacing=5;chart.groupSpacing=20
chart.barLabelFormat='%.3f';chart.barLabels.nudge=7
chart.barLabels.fontName='Helvetica';chart.barLabels.fontSize=10
for i,s in enumerate(series):
    chart.bars[i].fillColor=HexColor(s['color']);chart.bars[i].strokeColor=None
    x=478+i*156
    drawing.add(Rect(x,298,12,8,fillColor=HexColor(s['color']),strokeColor=None))
    drawing.add(String(x+18,297,s['label'],fontName='Helvetica',fontSize=10,fillColor=HexColor('#172b4d')))
drawing.add(String(44,298,'ns / write',fontName='Helvetica',fontSize=11,fillColor=HexColor('#475569')))
drawing.add(chart)
drawing.add(String(410,25,'Apple M3 | 8 MiB per layer | callback loops 0 | high QoS, no hard affinity',textAnchor='middle',fontName='Helvetica',fontSize=10,fillColor=HexColor('#475569')))
args.output.parent.mkdir(parents=True,exist_ok=True)
renderSVG.drawToFile(drawing,str(args.output))
namespace='http://www.w3.org/2000/svg'
ET.register_namespace('',namespace)
svg=ET.parse(args.output);root=svg.getroot()
title=ET.Element('{'+namespace+'}title');title.text='TwoBufQueue ordinary write and WriteSession group-average p50'
desc=ET.Element('{'+namespace+'}desc');desc.text=f'Matched parameters, Apple M3. Each bar is the mean of {repetitions} runs; quantiles of 1000-write averages, not single-call or end-to-end latency.'
root.insert(0,title);root.insert(1,desc);svg.write(args.output,encoding='utf-8',xml_declaration=True)
data={'metric':f'p50 of 1000-write group averages, {repetitions}-run arithmetic mean','repetitions':repetitions,'data_directory':args.data.name,'reportlab_version':reportlab.Version,'series':[{'mode':s['mode'],'points':[{'payload_bytes':g['payload_bytes'],'p50_ns':g['p50_ns'],'p50_runs_ns':g['p50_runs_ns']} for g in s['rows']]} for s in series]}
args.output.with_suffix('.json').write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
print(args.output)
