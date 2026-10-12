"""Export a self-contained publication tree; never delete source or destination files."""
import argparse
import datetime
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
ROOT_FILES = {'CMakeLists.txt','README.md','README.zh-CN.md','LICENSE','LICENSE.zh-CN.md',
              'NOTICE','CONTRIBUTING.md','NAMING.md','CHANGELOG.md','.gitignore','.gitattributes'}
BENCHMARK_FILES = {
    'spsc.cmake','spsc_dependencies.json','bootstrap_spsc.py','process_metrics.hpp',
    'tune_spsc.py','report_spsc.py','export_spsc_report.py','spsc_html_report.py',
    'run_release_benchmarks.py','plot_release.py','TUNABLE_SPSC.zh-CN.md','README.zh-CN.md',
    'BURST_CACHE.zh-CN.md','HOT_PATH_CACHE.zh-CN.md','block_size_round_budget.hpp',
    'burst_cache.hpp','burst_prefault.cpp','burst_replay.py','burst_showcase_report.py',
    'burst_tail_report.py','hot_path_cache.hpp','run_burst_cache.py','run_burst_showcase.py',
    'run_burst_tail_1m.py','test_burst_cache.cpp','test_hot_path_cache.cpp','test_hot_path_cache.py',
    'test_per_write_block_size_only_writer.cpp','test_per_write_block_size_only_writer.py',
    'test_per_writer_block_size.cpp','test_per_writer_block_size.py','tune_burst_business.py',
    'tune_three_writer_modes.py'}
HISTORICAL_TEST_DIRS = {'review_20260925','review_20260927','aba_review_20260928','results'}
TOOLS = {'export_release.py','verify_release.py','unpack_performance_data.py',
         'plot_burst_release.py','build_release_assets.py','build_pages.py'}
LINK = re.compile(r'(?P<html>(?:href|src)=["\'])(?P<url>[^"\']+)(?=["\'])|'
                  r'(?P<markdown>!?\[[^\]\n]*\]\()(?P<mdurl>[^)\n]+)(?=\))')


def included(relative):
    parts=relative.parts; name=relative.name
    if name=='.DS_Store' or '__pycache__' in parts or relative.suffix=='.pyc': return False
    if len(parts)==1: return name in ROOT_FILES
    if parts[0] in {'src','include','support','cmake'}:
        return relative.suffix in {'.cpp','.hpp','.cmake','.in'} or name=='CMakeLists.txt'
    if parts[0]=='examples': return relative.suffix in {'.cpp','.hpp','.md'}
    if parts[0]=='.github': return relative.suffix in {'.yml','.yaml','.md'}
    if parts[0]=='tools': return name in TOOLS
    if parts[0]=='test':
        if any(p in HISTORICAL_TEST_DIRS for p in parts) or name.endswith('RESULTS.md'): return False
        return relative.suffix in {'.cpp','.hpp','.py','.json','.cmake','.md'} or name=='CMakeLists.txt'
    if parts[0]=='benchmarks':
        if len(parts)==2: return name in BENCHMARK_FILES
        if parts[1]=='tunable': return relative.suffix in {'.hpp','.cpp'}
        if parts[1]=='profiles': return relative.suffix=='.json'
    if parts[0]=='docs':
        if len(parts)==2: return relative.suffix in {'.md','.html'}
        if parts[1]=='assets': return relative.suffix in {'.svg','.png','.json'}
        if parts[1]=='release-checks': return relative.suffix in {'.json','.log'}
    return False


def digest(data): return hashlib.sha256(data).hexdigest()


def local_target(document, reference):
    parsed=urlsplit(reference.strip('<>'))
    if parsed.scheme or parsed.netloc or not parsed.path: return None
    target=(document.parent/unquote(parsed.path)).resolve()
    if target==ROOT or ROOT not in target.parents:
        raise ValueError('Document link escapes source tree: '+reference)
    return target


def gather():
    selected={p.resolve() for p in ROOT.rglob('*') if p.is_file() and included(p.relative_to(ROOT))}
    queue=list(selected)
    for document in queue:
        if document.suffix not in {'.md','.html'} or 'performance_data' in document.parts: continue
        for match in LINK.finditer(document.read_text(encoding='utf-8')):
            reference=match['url'] or match['mdurl']; target=local_target(document,reference)
            if target is None: continue
            if target.is_dir():
                if any(target in p.parents for p in selected): continue
                target=next((target/n for n in ('index.html','README.md') if (target/n).is_file()),None)
                if target is None: raise ValueError('Directory link needs index: '+reference+' in '+str(document))
            if not target.is_file(): raise FileNotFoundError(str(document.relative_to(ROOT))+': '+reference)
            if target not in selected: selected.add(target); queue.append(target)
    # Saved files needed to regenerate the historical matched-parameter chart.
    data=ROOT/'docs/performance_data/release_20261011_inline_fixed_110024'
    for folder in ('8m_normal','8m_session'):
        p=data/folder/'results.json'
        if p.is_file():selected.add(p)
    # This compact bundle's JSON index refers to nested, already compressed
    # instruction-counter samples and source excerpts rather than direct URLs.
    evidence=ROOT/'docs/performance_data/burst_instruction_1m_20261012'
    if evidence.exists():
        selected.update(p for p in evidence.rglob('*') if p.is_file())
    return sorted(selected)


def gzip_bytes(data):
    output=io.BytesIO()
    with gzip.GzipFile(filename='',fileobj=output,mode='wb',mtime=0,compresslevel=9) as stream: stream.write(data)
    packed=output.getvalue()
    if gzip.decompress(packed)!=data: raise IOError('Compression verification failed')
    return packed


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination',type=Path)
    parser.add_argument('--update',action='store_true',help='Update exported files; never delete extras')
    args=parser.parse_args(); destination=args.destination.expanduser().resolve()
    if destination==ROOT or ROOT in destination.parents or destination in ROOT.parents:
        parser.error('Destination must be separate from source tree')
    for name in ROOT_FILES:
        if not (ROOT/name).is_file(): raise FileNotFoundError(name)
    entries={}; packed_paths={}
    existing=ROOT/'compression-manifest.json'
    compression=json.loads(existing.read_text())['files'] if existing.is_file() else []
    for source in gather():
        if source.is_symlink(): raise ValueError('Unexpected source symlink: '+str(source))
        relative=source.relative_to(ROOT); original=source.read_bytes(); data=original
        packed='performance_data' in relative.parts and source.suffix in {'.json','.jsonl','.csv'} and len(data)>1024*1024
        output=Path(str(relative)+'.gz') if packed else relative
        if packed:
            data=gzip_bytes(original); packed_paths[source]=output
            compression.append({'original_path':relative.as_posix(),'compressed_path':output.as_posix(),
                'original_bytes':len(original),'compressed_bytes':len(data),
                'original_sha256':digest(original),'compressed_sha256':digest(data)})
        entries[output]={'data':data,'source':relative.as_posix(),'source_sha256':digest(original)}
    for relative,entry in entries.items():
        if relative.suffix not in {'.md','.html'} or 'performance_data' in relative.parts: continue
        document=ROOT/entry['source']
        def rewrite(match):
            reference=match['url'] or match['mdurl']; target=local_target(document,reference)
            if target in packed_paths:
                parsed=urlsplit(reference); replacement=reference.replace(parsed.path,parsed.path+'.gz',1)
                return match.group(0).replace(reference,replacement,1)
            return match.group(0)
        entry['data']=LINK.sub(rewrite,entry['data'].decode('utf-8')).encode('utf-8')
    cm={'schema_version':1,'encoding':'gzip','lossless':True,'files':compression}
    entries[Path('compression-manifest.json')]={'data':(json.dumps(cm,indent=2)+'\n').encode()}
    selected_paths=set(entries)|{Path('release-manifest.json')}
    extras=sorted(p.relative_to(destination) for p in destination.rglob('*') if p.is_file()
        and '.git' not in p.relative_to(destination).parts and p.relative_to(destination) not in selected_paths)
    ignored=['\n# Preserved local files outside the publication manifest.\n','/.release-assets/\n','/.release-validation/\n']
    original_paths={record['original_path'] for record in compression}
    for original in sorted(original_paths): ignored.append('/'+original+'\n')
    for extra in extras:
        text=extra.as_posix()
        if text in original_paths or text.startswith(('.release-assets/','.release-validation/')): continue
        ignored.append('/'+re.sub(r'([ *?\[\]#!\\])',r'\\\1',text)+'\n')
    entries[Path('.gitignore')]['data']+=''.join(ignored).encode()
    for relative,entry in entries.items():
        target=destination/relative
        if target.exists() and (not target.is_file() or target.read_bytes()!=entry['data']) and not args.update:
            raise FileExistsError('Destination differs: '+str(target)+'; inspect before --update')
    destination.mkdir(parents=True,exist_ok=True); records=[]
    for relative,entry in sorted(entries.items()):
        target=destination/relative; target.parent.mkdir(parents=True,exist_ok=True); target.write_bytes(entry['data'])
        if digest(target.read_bytes())!=digest(entry['data']): raise IOError('Copy checksum mismatch: '+str(relative))
        record={'path':relative.as_posix(),'bytes':len(entry['data']),'sha256':digest(entry['data'])}
        if 'source' in entry: record.update({'source_path':entry['source'],'source_sha256':entry['source_sha256']})
        records.append(record)
    manifest={'schema_version':2,'created_at':datetime.datetime.now().astimezone().isoformat(),
        'source_tree_modified_by_export':False,'git_history_copied':False,'file_count':len(records),
        'total_bytes':sum(x['bytes'] for x in records),'preserved_unselected_files':[p.as_posix() for p in extras],
        'files':records}
    (destination/'release-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({'destination':str(destination),'files':len(records),'bytes':manifest['total_bytes'],
        'compressed_evidence_files':len(compression),'preserved_unselected_files':len(extras)},ensure_ascii=False))


if __name__=='__main__': main()
