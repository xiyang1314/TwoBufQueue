"""Create source and measurement release archives from the publication allowlist."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import tarfile
import shutil
import zipfile
from html import escape

ROOT = Path(__file__).resolve().parents[1]


def archive(target, paths, prefix):
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open('wb') as raw:
        with gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w') as output:
                for path in sorted(paths):
                    info = output.gettarinfo(str(path), prefix + '/' + path.relative_to(ROOT).as_posix())
                    info.mtime = 0
                    info.uid = info.gid = 0
                    info.uname = info.gname = ''
                    with path.open('rb') as stream:
                        output.addfile(info, stream)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', default='0.1.0')
    parser.add_argument('--output', type=Path, default=ROOT / 'packages')
    args = parser.parse_args()
    if not args.version or any(c not in '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-' for c in args.version):
        parser.error('Invalid version')
    output = args.output.resolve()
    manifest = ROOT / 'release-manifest.json'
    if not manifest.is_file():
        parser.error('Run this tool from the exported publication tree')
    records = json.loads(manifest.read_text())['files']
    paths = [ROOT / r['path'] for r in records] + [manifest]
    prefix = 'TwoBufQueue-' + args.version
    source = output / (prefix + '-source.tar.gz')
    evidence = output / (prefix + '-performance-data.tar.gz')
    archive(source, paths, prefix)
    data = [p for p in paths if 'performance_data' in p.relative_to(ROOT).parts or p.name == 'compression-manifest.json']
    archive(evidence, data, prefix)
    language_packages = []
    canonical_source = {
        p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in paths if p.relative_to(ROOT).parts[0] in {'src', 'include', 'support'}
        or (p.relative_to(ROOT).parts[0] == 'examples' and p.suffix in {'.cpp', '.hpp'})
    }
    for language in ('EN', 'CN'):
        name = prefix + '-' + language
        folder = output / name
        folder.mkdir(parents=True, exist_ok=True)
        selected = []
        for path in paths:
            relative = path.relative_to(ROOT)
            target = folder / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            if hashlib.sha256(target.read_bytes()).digest() != hashlib.sha256(path.read_bytes()).digest():
                raise IOError('Package copy differs: ' + str(relative))
            selected.append(target)
        entry = 'README.html' if language == 'EN' else 'README.zh-CN.html'
        info = {'schema_version': 1, 'version': args.version, 'default_language': language,
                'entry': entry, 'canonical_source_sha256': canonical_source,
                'source_policy': 'Generated from one canonical tree; C++ source is identical in EN and CN packages.'}
        extras = {
            'index.html': '<!doctype html><html lang="' + ('en' if language == 'EN' else 'zh-CN')
                + '"><meta charset="utf-8"><meta http-equiv="refresh" content="0;url=' + entry
                + '"><title>TwoBufQueue ' + language + '</title><a href="' + entry
                + '">' + ('Open English documentation' if language == 'EN' else '打开中文文档') + '</a></html>\n',
            'OPEN_FIRST.txt': ('Open index.html or README.html to read the English documentation.\n'
                'The CN and EN packages use identical library/example code and contain both documentation languages.\n'
                'For contributions, edit the canonical repository rather than maintaining a language fork.\n')
                if language == 'EN' else ('请先打开 index.html 或 README.zh-CN.html 阅读中文文档。\n'
                'CN 和 EN 包使用完全相同的库与示例代码，并同时包含中英文文档。\n'
                '提交功能修改时请修改正式仓库，不要维护单独的语言分支。\n'),
            'PACKAGE_INFO.json': json.dumps(info, ensure_ascii=False, indent=2) + '\n',
        }
        for filename, text in extras.items():
            target = folder / filename
            target.write_text(text, encoding='utf-8')
            selected.append(target)
        archive_path = output / (name + '.zip')
        with zipfile.ZipFile(archive_path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zipped:
            for path in sorted(selected):
                item = zipfile.ZipInfo(name + '/' + path.relative_to(folder).as_posix(), date_time=(1980, 1, 1, 0, 0, 0))
                item.compress_type = zipfile.ZIP_DEFLATED
                item.external_attr = 0o100644 << 16
                zipped.writestr(item, path.read_bytes())
        with zipfile.ZipFile(archive_path) as zipped:
            if zipped.testzip() is not None:
                raise IOError('ZIP validation failed: ' + str(archive_path))
            for relative, sha in canonical_source.items():
                if hashlib.sha256(zipped.read(name + '/' + relative)).hexdigest() != sha:
                    raise IOError('Canonical source mismatch: ' + relative)
        language_packages.append({'language': language, 'directory': str(folder), 'zip': str(archive_path),
                                  'sha256': hashlib.sha256(archive_path.read_bytes()).hexdigest(),
                                  'entry': entry, 'files': len(selected)})
    index = '<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>TwoBufQueue packages</title><style>body{font:17px/1.7 system-ui;max-width:880px;margin:50px auto;padding:0 20px;background:#f3f6fa;color:#193348}section{background:white;border:1px solid #d5e2eb;padding:26px;border-radius:16px;margin:20px 0}a{color:#087e68}h1{margin-bottom:8px}code{font-size:12px;overflow-wrap:anywhere}</style><h1>TwoBufQueue ' + escape(args.version) + '</h1><p>One source tree, two documentation defaults. / 同一套源码，两种默认文档语言。</p>'
    for package in language_packages:
        title = 'English package' if package['language'] == 'EN' else '中文包'
        archive_name = Path(package['zip']).name
        folder_name = Path(package['directory']).name
        index += '<section><h2>' + title + '</h2><p><a href="' + escape(archive_name) + '">Download ZIP / 下载 ZIP</a> · <a href="' + escape(folder_name + '/index.html') + '">Open documentation / 打开文档</a></p><code>SHA-256: ' + package['sha256'] + '</code></section>'
    (output / 'index.html').write_text(index + '</html>\n', encoding='utf-8')
    packaged = [source, evidence] + [Path(p['zip']) for p in language_packages]
    sums = ''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n' for p in packaged)
    (output / 'SHA256SUMS').write_text(sums)
    print(json.dumps({'source': str(source), 'performance_data': str(evidence), 'sha256sums': str(output / 'SHA256SUMS'), 'language_packages': language_packages, 'package_index': str(output / 'index.html')}))


if __name__ == '__main__':
    main()
