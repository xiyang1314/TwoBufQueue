"""Restore losslessly packed measurements; refuse to overwrite different files."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def checked_path(root, name):
    path = (root / name).resolve()
    if root.resolve() not in path.parents:
        raise ValueError('Path escapes destination: ' + name)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, default=ROOT)
    args = parser.parse_args()
    destination = args.destination.resolve()
    manifest = json.loads((ROOT / 'compression-manifest.json').read_text())
    for record in manifest['files']:
        source = checked_path(ROOT, record['compressed_path'])
        packed = source.read_bytes()
        if hashlib.sha256(packed).hexdigest() != record['compressed_sha256']:
            raise ValueError('Compressed checksum mismatch: ' + str(source))
        data = gzip.decompress(packed)
        if len(data) != record['original_bytes'] or hashlib.sha256(data).hexdigest() != record['original_sha256']:
            raise ValueError('Original checksum mismatch: ' + str(source))
        target = checked_path(destination, record['original_path'])
        if target.exists() and target.read_bytes() != data:
            raise FileExistsError('Different destination file: ' + str(target))
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            target.write_bytes(data)
    print(json.dumps({'restored_or_verified_files': len(manifest['files']), 'destination': str(destination)}))


if __name__ == '__main__':
    main()
