"""Regenerate the homepage batch/business figure from saved measurements (ReportLab 4.4.9)."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import xml.etree.ElementTree as ET
from reportlab.graphics.charts.barcharts import HorizontalBarChart
from reportlab.graphics.shapes import Drawing, Line, Rect, String
from reportlab.graphics import renderSVG
from reportlab.lib.colors import HexColor

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data',type=Path,default=ROOT/'docs/performance_data/burst_from_empty_20261013/formal/report_results.json')
    parser.add_argument('--output',type=Path,default=ROOT/'docs/assets/burst_business_cost.svg')
    args=parser.parse_args(); source=args.data.read_bytes(); result=json.loads(source)
    bursts=[2,4,8,16]; queues=['atomic_queue','rigtorp','ReaderWriterQueue','TwoBufQueue']
    labels=['atomic_queue','rigtorp::SPSCQueue','ReaderWriterQueue','TwoBufQueue']
    rows=[r for r in result['rows'] if r['burst_mib'] in bursts and r['queue'] in queues]
    if len(rows)!=16 or not result['verified']:raise ValueError('Incomplete or unverified saved results')
    empty=result['parameters'].get('start_mode')=='empty_each_burst'
    normal_ns=round(statistics.mean(r['p50'] for r in result['rows'] if r['queue']=='baseline' and r['burst_mib'] in bursts))
    drawing=Drawing(1040,510)
    drawing.add(Rect(0,0,1040,510,fillColor=HexColor('#f3f6fb'),strokeColor=None))
    title_text='Business performance immediately after a message batch' if empty else 'Business execution with a prepared message backlog'
    state_text='Every batch starts empty.' if empty else 'A large backlog is prepared before each batch.'
    drawing.add(String(24,477,title_text,fontName='Helvetica-Bold',fontSize=23,fillColor=HexColor('#183048')))
    drawing.add(String(24,452,state_text+' Same scale in all panels. Lower is better. Dashed line: normal business ('+str(normal_ns)+' ns/call).',fontName='Helvetica',fontSize=12,fillColor=HexColor('#576d83')))
    for panel,burst in enumerate(bursts):
        x=20+(panel%2)*515; y=246-(panel//2)*212
        drawing.add(Rect(x,y,495,194,rx=12,ry=12,fillColor=HexColor('#ffffff'),strokeColor=HexColor('#dce5ee')))
        drawing.add(String(x+18,y+166,str(burst)+' MiB per burst',fontName='Helvetica-Bold',fontSize=15,fillColor=HexColor('#183048')))
        values=[next(r['p50'] for r in rows if r['queue']==queue and r['burst_mib']==burst) for queue in queues]
        chart=HorizontalBarChart();chart.x=x+155;chart.y=y+35;chart.width=294;chart.height=112
        chart.data=[values];chart.categoryAxis.categoryNames=labels
        chart.categoryAxis.labels.fontName='Helvetica';chart.categoryAxis.labels.fontSize=10
        chart.categoryAxis.labels.fillColor=HexColor('#183048');chart.categoryAxis.strokeColor=None
        chart.valueAxis.valueMin=0;chart.valueAxis.valueMax=120;chart.valueAxis.valueStep=40
        chart.valueAxis.labels.fontName='Helvetica';chart.valueAxis.labels.fontSize=9
        chart.valueAxis.labels.fillColor=HexColor('#576d83');chart.valueAxis.strokeColor=HexColor('#c5d3df')
        chart.valueAxis.visibleGrid=True;chart.valueAxis.gridStrokeColor=HexColor('#e4ebf1')
        chart.barWidth=14;chart.barLabelFormat='%.1f';chart.barLabels.fontName='Helvetica';chart.barLabels.fontSize=10;chart.barLabels.nudge=6
        chart.bars[0].fillColor=HexColor('#6586a4');chart.bars[0].strokeColor=None
        chart.bars[0,3].fillColor=HexColor('#087f6e')
        drawing.add(chart)
        baseline_x=chart.x+normal_ns/chart.valueAxis.valueMax*chart.width
        drawing.add(Line(baseline_x,chart.y,baseline_x,chart.y+chart.height,strokeColor=HexColor('#86552d'),strokeWidth=1,strokeDashArray=[3,3]))
        drawing.add(String(x+155,y+13,'Business cost (ns/call)',fontName='Helvetica',fontSize=10,fillColor=HexColor('#576d83')))
    drawing.add(String(24,12,'Apple M3 | 32 B records | 48 KiB business data | p50 of 16-call group means | 3 independent runs',fontName='Helvetica',fontSize=11,fillColor=HexColor('#576d83')))
    args.output.parent.mkdir(parents=True,exist_ok=True);renderSVG.drawToFile(drawing,str(args.output))
    namespace='http://www.w3.org/2000/svg';ET.register_namespace('',namespace)
    svg=ET.parse(args.output);node=svg.getroot();title=ET.Element('{'+namespace+'}title');title.text='Business group-average p50 after message bursts'
    desc=ET.Element('{'+namespace+'}desc');desc.text=state_text+' Four burst sizes, all queues, identical horizontal scale. Mean of three runs of p50 of 16-call business group averages. Dashed line: normal business '+str(normal_ns)+' ns/call.'
    node.insert(0,title);node.insert(1,desc);svg.write(args.output,encoding='utf-8',xml_declaration=True)
    source_path=args.data.resolve()
    try:source_name=source_path.relative_to(ROOT).as_posix()
    except ValueError:source_name=source_path.as_posix()
    chart_data={'source_sha256':hashlib.sha256(source).hexdigest(),'source':source_name,
        'start_mode':'empty_each_burst' if empty else 'prefilled', 'normal_business_ns':normal_ns,
        'metric':'mean of three runs of business 16-call group-average p50 in ns/call','rows':rows}
    args.output.with_suffix('.json').write_text(json.dumps(chart_data,indent=2)+'\n');print(args.output)


if __name__=='__main__':main()
