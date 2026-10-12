"""Build a static Pages tree from publication files, preserving relative evidence links."""
import argparse
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output == ROOT or output in ROOT.parents:
        parser.error('Output must be separate from the source root')
    manifest = json.loads((ROOT / 'release-manifest.json').read_text())
    selected = {record['path'] for record in manifest['files']}
    selected.add('release-manifest.json')
    allowed = selected | {'index.html', '.nojekyll'}
    for path in output.rglob('*'):
        if path.is_file() and path.relative_to(output).as_posix() not in allowed:
            raise FileExistsError('Output contains an unrelated file; choose an empty directory: ' + str(path))
    output.mkdir(parents=True, exist_ok=True)
    for name in sorted(selected):
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, target)
    (output / '.nojekyll').write_text('')
    (output / 'index.html').write_text('<!doctype html><html lang="en"><meta charset="utf-8"><meta http-equiv="refresh" content="0; url=docs/index.html"><title>TwoBufQueue reports</title><a href="docs/index.html">Open TwoBufQueue performance reports</a></html>\n')
    print(json.dumps({'output': str(output), 'files': len(selected) + 2}))


if __name__ == '__main__':
    main()
