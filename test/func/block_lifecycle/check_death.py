import subprocess
import sys
r = subprocess.run(sys.argv[1:3], capture_output=True, text=True)
assert r.returncode == 91, (r.returncode, r.stdout, r.stderr)
assert sys.argv[3] in r.stderr, r.stderr
print('PASS', sys.argv[2])
