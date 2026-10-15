"""Build ZIP and tar.gz source archives, each containing both documentation languages."""
import argparse
import gzip
import hashlib
from html import escape
import json
from pathlib import Path
import re
import shutil
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def archive(target, paths, prefix):
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
    version = re.search(r'project\(TwoBufQueue\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)', (ROOT / 'CMakeLists.txt').read_text())
    if not version:
        raise ValueError('Missing project version in CMakeLists.txt')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', default=version[1])
    parser.add_argument('--output', type=Path, default=ROOT / 'packages')
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9.-]+)?', args.version):
        parser.error('Invalid version')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = ROOT / 'release-manifest.json'
    records = json.loads(manifest_path.read_text())['files']
    paths = [ROOT / record['path'] for record in records] + [manifest_path]
    expected = {path.relative_to(ROOT).as_posix(): digest(path.read_bytes()) for path in paths}
    for entry in ('en/index.html', 'zh_cn/index.html', 'START_HERE.html'):
        if entry not in expected:
            parser.error('Generate language directories and refresh the release manifest first: ' + entry)
    prefix = 'TwoBufQueue-' + args.version
    folder = output / prefix
    folder.mkdir(parents=True, exist_ok=True)
    for path in paths:
        target = folder / path.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    zipped_path = output / (prefix + '.zip')
    with zipfile.ZipFile(zipped_path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zipped:
        for path in sorted(paths):
            item = zipfile.ZipInfo(prefix + '/' + path.relative_to(ROOT).as_posix(), date_time=(1980, 1, 1, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            item.external_attr = (path.stat().st_mode & 0o777 | 0o100000) << 16
            zipped.writestr(item, path.read_bytes())
    tar_path = output / (prefix + '.tar.gz')
    archive(tar_path, paths, prefix)
    with zipfile.ZipFile(zipped_path) as zipped:
        if zipped.testzip() is not None:
            raise IOError('ZIP integrity check failed')
        assert set(zipped.namelist()) == {prefix + '/' + name for name in expected}
        for name, sha in expected.items():
            assert digest(zipped.read(prefix + '/' + name)) == sha, name
    with tarfile.open(tar_path, 'r:gz') as packed:
        members = {member.name: member for member in packed.getmembers() if member.isfile()}
        assert set(members) == {prefix + '/' + name for name in expected}
        for name, sha in expected.items():
            assert digest(packed.extractfile(members[prefix + '/' + name]).read()) == sha, name
    assets = [{'file': path.name, 'sha256': digest(path.read_bytes()), 'bytes': path.stat().st_size} for path in (zipped_path, tar_path)]
    index = '<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>TwoBufQueue packages</title><style>body{font:17px/1.7 system-ui;max-width:900px;margin:48px auto;padding:0 20px;background:#f3f6fa;color:#193348}section{background:white;border:1px solid #d5e2eb;padding:26px;border-radius:16px;margin:20px 0}a{color:#087e68}code{font-size:12px;overflow-wrap:anywhere}</style><h1>TwoBufQueue ' + escape(args.version) + '</h1><p>Both archives contain one shared source tree and separate <code>zh_cn/</code> and <code>en/</code> documentation directories. 两个包都包含中英文文档，内容相同，仅压缩格式不同。</p>'
    for asset in assets:
        index += '<section><h2><a href="' + escape(asset['file']) + '">' + escape(asset['file']) + '</a></h2><code>SHA-256: ' + asset['sha256'] + '</code></section>'
    index += '<p><a href="' + prefix + '/START_HERE.html">Choose a language / 选择语言</a> · <a href="' + prefix + '/zh_cn/index.html">中文文档</a> · <a href="' + prefix + '/en/index.html">English documentation</a></p></html>\n'
    (output / 'index.html').write_text(index, encoding='utf-8')
    print(json.dumps({'version': args.version, 'archives': assets, 'files_per_archive': len(expected), 'directory': str(folder),
                      'publication': 'GitHub Releases use only the two automatic Source code archives; no manual assets are required.'}))


if __name__ == '__main__':
    main()
