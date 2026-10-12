import argparse
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--exe', required=True)
parser.add_argument('--catalog', required=True, type=Path)
args = parser.parse_args()
cases = json.loads(args.catalog.read_text(encoding='utf-8'))
ids = [case['id'] for case in cases]
if len(ids) != len(set(ids)):
    raise SystemExit('Duplicate case ID')
for case in cases:
    for field in ('id', 'point', 'setup', 'steps', 'expected'):
        if not isinstance(case.get(field), str) or not case[field].strip():
            raise SystemExit('Incomplete case: ' + str(case))
rows = ['|' + '|'.join(c[k] for k in ('id','point','setup','steps','expected')) + '|' for c in cases]
document = (args.catalog.parent/'TEST_CASES.md').read_text(encoding='utf-8')
if rows != [line for line in document.splitlines() if line.startswith('|')][2:]:
    raise SystemExit('Document/catalog mismatch')
result = subprocess.run([args.exe, '--list'], text=True, capture_output=True, check=True, timeout=10)
actual = result.stdout.splitlines()
if len(actual) != len(set(actual)) or set(actual) != set(ids):
    raise SystemExit(f'Registration mismatch: missing={set(ids)-set(actual)}, extra={set(actual)-set(ids)}')
print(f'PASS catalog: {len(ids)} documented concurrent data cases')
