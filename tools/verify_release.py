"""Check publication hashes, local document links and lossless evidence packing."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
from document_links import document_links


def verify(root, check_manifest=True):
    manifest = root / 'release-manifest.json'
    records = json.loads(manifest.read_text(encoding='utf-8'))['files'] if manifest.exists() else []
    paths = [root / r['path'] for r in records] if records else [p for p in root.rglob('*') if p.is_file()
        and '.git' not in p.relative_to(root).parts and 'performance_archive' not in p.parts
        and not any(x.startswith('build') for x in p.relative_to(root).parts)]
    reading_path = root / 'tools/github_reading_pages.json'
    reading_pages = {page['markdown'] for page in json.loads(reading_path.read_text(encoding='utf-8'))['pages']} if reading_path.exists() else set()
    errors = []
    dependency_manifest = root / 'benchmarks/spsc/spsc_dependencies.json'
    if dependency_manifest.is_file():
        dependencies = json.loads(dependency_manifest.read_text(encoding='utf-8'))
        published = {record['path'] for record in records}
        for name, dependency in dependencies.items():
            for entry in dependency['files']:
                relative = 'third_party/spsc/' + name + '/' + entry['path']
                path = root / relative
                if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
                    errors.append('Bundled dependency hash mismatch: ' + relative)
                if records and relative not in published:
                    errors.append('Bundled dependency missing from publication manifest: ' + relative)
    for record in records if check_manifest else []:
        path = root / record['path']
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != record['sha256']:
            errors.append('Manifest mismatch: ' + record['path'])
    checked = 0
    for document in paths:
        if document.suffix not in {'.md', '.html'}:
            continue
        body = document.read_text(encoding='utf-8')
        if document.relative_to(root).parts[:2] == ('docs', 'performance_data') and document.relative_to(root).as_posix() not in reading_pages:
            continue
        if document.suffix == '.md' and ('README_TOC:' in body or body.startswith('<!-- Generated GitHub reading copy from ')):
            errors.append('Internal generation marker in a reading document: ' + str(document.relative_to(root)))
        for match in document_links(document, body):
            reference = (match['url'] or match['mdurl']).strip('<>')
            parsed = urlsplit(reference)
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            if document.suffix == '.md' and parsed.path.lower().endswith('.html'):
                errors.append('Markdown link opens HTML source in the repository: ' + str(document.relative_to(root)) + ': ' + reference)
            target = (document.parent / unquote(parsed.path)).resolve()
            checked += 1
            if not target.exists():
                errors.append('Missing link in ' + str(document.relative_to(root)) + ': ' + reference)
            elif root != target and root not in target.parents:
                errors.append('Link escapes publication tree: ' + reference)
    compression = root / 'compression-manifest.json'
    packed = json.loads(compression.read_text(encoding='utf-8'))['files'] if compression.exists() else []
    for record in packed:
        path = root / record['compressed_path']
        if not path.is_file():
            errors.append('Missing compressed evidence: ' + record['compressed_path'])
            continue
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != record['compressed_sha256']:
            errors.append('Compressed evidence hash mismatch: ' + record['compressed_path'])
        original = gzip.decompress(data)
        if hashlib.sha256(original).hexdigest() != record['original_sha256'] or len(original) != record['original_bytes']:
            errors.append('Lossless evidence check failed: ' + record['original_path'])
    oversized = [str(p.relative_to(root)) for p in paths if p.stat().st_size > 50 * 1024 * 1024]
    errors.extend('Publication file exceeds 50 MiB: ' + p for p in oversized)
    if errors:
        raise ValueError('\n'.join(errors))
    return {'manifest_files': len(records), 'local_links_checked': checked,
            'compressed_files_verified': len(packed), 'files_over_50_mib': len(oversized), 'passed': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--skip-manifest', action='store_true', help='Validate links/data in working changes without enforcing the export snapshot hashes')
    args = parser.parse_args()
    result = verify(args.root.resolve(), not args.skip_manifest)
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding='utf-8')
    print(text, end='')


if __name__ == '__main__':
    main()
