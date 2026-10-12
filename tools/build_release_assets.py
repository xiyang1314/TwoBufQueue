"""Create source and measurement release archives from the publication allowlist."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import tarfile

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
    parser.add_argument('--output', type=Path, default=ROOT / '.release-assets')
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
    sums = ''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n' for p in (source, evidence))
    (output / 'SHA256SUMS').write_text(sums)
    print(json.dumps({'source': str(source), 'performance_data': str(evidence), 'sha256sums': str(output / 'SHA256SUMS')}))


if __name__ == '__main__':
    main()
