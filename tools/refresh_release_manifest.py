"""Refresh a canonical publication manifest without exporting or deleting files."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
from urllib.parse import unquote, urlsplit
from document_links import document_links

ROOT = Path(__file__).resolve().parents[1]


def refresh(root, check=False):
    root = root.resolve()
    manifest_path = root / 'release-manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    previous = {record['path']: record for record in manifest['files']}
    if len(previous) != len(manifest['files']):
        raise ValueError('Duplicate publication path')
    selected = set(previous)
    dependencies = json.loads((root / 'benchmarks/spsc/spsc_dependencies.json').read_text(encoding='utf-8'))
    for name, dependency in dependencies.items():
        for entry in dependency['files']:
            relative = 'third_party/spsc/' + name + '/' + entry['path']
            path = root / relative
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
                raise ValueError('Bundled dependency differs from pinned upstream file: ' + relative)
            selected.add(relative)
    documents = json.loads((root / 'tools/documentation_pages.json').read_text(encoding='utf-8'))
    for source in documents['sources']:
        if not source.endswith('.md'):
            raise ValueError('Expected a Markdown source: ' + source)
        selected.update((source, source[:-3] + '.html'))
    code = json.loads((root / 'docs/code/code-manifest.json').read_text(encoding='utf-8'))
    selected.update(('tools/refresh_release_manifest.py', 'docs/code/index.html', 'docs/code/code-manifest.json'))
    for record in code['files']:
        selected.update((record['source_path'], record['html_path']))
        if record.get('html_path_zh_cn'):
            selected.add(record['html_path_zh_cn'])

    selected.update(('docs/code/index.zh-CN.html', 'START_HERE.html'))
    for name in json.loads((root / 'tools/report_pages.json').read_text())['reports']:
        selected.update((name, name[:-5] + '.zh-CN.html'))
    selected.update(('tools/localize_reading_pages.py', 'tools/report_translations.json', 'tools/report_pages.json'))
    github = json.loads((root / 'tools/github_reading_pages.json').read_text())
    selected.update(('tools/build_github_reading.py', 'tools/github_reading_pages.json', 'tools/document_sections.py', 'tools/document_site.json', 'tools/document_site.mjs'))
    selected.update(page['markdown'] for page in github['pages'])
    selected.update(github['assets'])
    # Include the evidence files linked by generated reading copies as well.
    for page in github['pages']:
        document = root / page['markdown']
        for match in document_links(document, document.read_text(encoding='utf-8')):
            parsed = urlsplit((match['url'] or match['mdurl']).strip('<>'))
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            target = (document.parent / unquote(parsed.path)).resolve()
            if root not in target.parents or not target.is_file():
                raise ValueError('Missing or unsafe reading-copy link: ' + str(target))
            selected.add(target.relative_to(root).as_posix())
    records = []
    changed = []
    added = []
    for name in sorted(selected):
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts or relative.as_posix() != name:
            raise ValueError('Non-canonical publication path: ' + name)
        if name == 'release-manifest.json' or any(part.startswith('.') for part in relative.parts[:-1]):
            # Existing workflow files are part of the original explicit allowlist.
            if name not in previous or relative.parts[0] != '.github':
                raise ValueError('Unexpected hidden or self-referential path: ' + name)
        path = root / relative
        if path.is_symlink() or root not in path.resolve().parents or not path.is_file():
            raise ValueError('Missing or unsafe publication file: ' + name)
        data = path.read_bytes()
        record = dict(previous.get(name, {'path': name}))
        record.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        if name not in previous:
            added.append(name)
        elif record != previous[name]:
            changed.append(name)
        records.append(record)

    result = {'manifest_files': len(records), 'changed_files': len(changed), 'added_files': added,
              'check_only': check, 'up_to_date': not changed and not added}
    if not check and not result['up_to_date']:
        manifest.update(files=records, file_count=len(records), total_bytes=sum(r['bytes'] for r in records),
                        refreshed_at=datetime.datetime.now().astimezone().isoformat(),
                        refresh_policy='bytes/sha256 describe current canonical files; '
                        'source_path/source_sha256 retain original export provenance where present.')
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--check', action='store_true', help='Report stale records without writing files')
    args = parser.parse_args()
    result = refresh(args.root, args.check)
    print(json.dumps(result, indent=2))
    if args.check and not result['up_to_date']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
