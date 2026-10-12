"""Check case traceability or a contract failure in an isolated process."""

import argparse
import json
import subprocess
import sys
from pathlib import Path
from verify_compile import CASES as COMPILE_CASES


def verify_catalog(executable, path):
    cases = json.loads(path.read_text(encoding="utf-8"))
    ids = [case["id"] for case in cases]
    if len(ids) != len(set(ids)):
        sys.exit("Duplicate ID in cases.json")
    for case in cases:
        for field in ("id", "point", "setup", "steps", "expected", "kind"):
            if not isinstance(case.get(field), str) or not case[field].strip():
                sys.exit(f"Missing case field {field}: {case}")
        if case["kind"] not in {"runtime", "stress", "death", "compile"}:
            sys.exit(f"Unknown case kind: {case}")
    if {case["id"] for case in cases if case["kind"] == "compile"} != set(COMPILE_CASES):
        sys.exit("Compile case catalog differs from verify_compile.py")

    # Keep the reviewable document and machine-readable specification identical.
    document = (path.parent / "TEST_CASES.md").read_text(encoding="utf-8")
    rows = [line for line in document.splitlines() if line.startswith("|")][2:]
    expected_rows = [
        "|" + "|".join(case[key] for key in ("id", "point", "setup", "steps", "expected")) + "|"
        for case in cases
    ]
    if rows != expected_rows:
        sys.exit("TEST_CASES.md differs from cases.json")

    expected = {case["id"] for case in cases if case["kind"] != "compile"}
    result = subprocess.run(
        [executable, "--list"], capture_output=True, text=True, timeout=10, check=True
    )
    actual = result.stdout.splitlines()
    if len(actual) != len(set(actual)) or set(actual) != expected:
        sys.exit(f"Catalog mismatch: missing={expected-set(actual)}, extra={set(actual)-expected}")
    print(f"PASS catalog: {len(actual)} runtime/death cases, {len(cases)-len(actual)} compile cases")


def verify_death(executable, case_id):
    result = subprocess.run(
        [executable, "--case", case_id], capture_output=True, text=True, timeout=15
    )
    # The child verifies the exact diagnostic before returning this sentinel.
    if result.returncode != 86 or "EXPECTED_CONTRACT_FAILURE" not in result.stderr:
        sys.exit(f"Wrong death result {result.returncode}: {result.stdout}\n{result.stderr}")
    print("PASS expected contract failure", case_id)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--case")
    mode.add_argument("--catalog", type=Path)
    args = parser.parse_args()
    if args.catalog:
        verify_catalog(args.exe, args.catalog)
    else:
        verify_death(args.exe, args.case)


if __name__ == "__main__":
    main()
