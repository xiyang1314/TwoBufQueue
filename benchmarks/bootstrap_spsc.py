#!/usr/bin/env python3
"""Fetch only pinned SPSC benchmark headers and licenses; never execute downloads."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import urllib.request


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fetch(dep, entry):
    headers = {"User-Agent": "TwoBufQueue-SPSC-benchmark"}
    try:
        request = urllib.request.Request(entry["url"], headers=headers)
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.read()
    except OSError:
        url = "https://api.github.com/repos/{}/contents/{}?ref={}".format(
            dep["repository"], entry["path"], dep["commit"])
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as response:
            return base64.b64decode(json.load(response)["content"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / ".deps/spsc")
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    manifest = json.loads(Path(__file__).with_name("spsc_dependencies.json").read_text())
    for name, dep in manifest.items():
        for entry in dep["files"]:
            target = args.output / name / entry["path"]
            if target.exists():
                if digest(target) != entry["sha256"]:
                    raise RuntimeError("SHA256 mismatch: {}".format(target))
                continue
            if args.check_only:
                raise RuntimeError("Missing dependency: {}".format(target))
            data = fetch(dep, entry)
            if hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise RuntimeError("Downloaded SHA256 mismatch: {}".format(target))
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_name(target.name + ".download")
            temporary.write_bytes(data)
            temporary.replace(target)
            print("Fetched", name, entry["path"], flush=True)
    print("Pinned SPSC dependencies verified:", args.output.resolve())


if __name__ == "__main__":
    main()
