# Release contents

[English](https://xiyang1314.github.io/TwoBufQueue/docs/development/RELEASE_CONTENTS.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/docs/development/RELEASE_CONTENTS.zh-CN.html)

The release directory contains current library source and headers, CMake package support, runnable examples, active regression/functional tests, CI, current SPSC comparisons and block-size/backlog tests, published reports and their referenced evidence. LICENSE and NOTICE remain at the root.

Pinned comparison headers and their original licenses are included under `third_party/spsc/`, together with version and hash information. Publication excludes temporary notes, directory copies, `.DS_Store`, build outputs and optional download caches. Selection never deletes files in the original project or destination; existing destination extras remain ignored by Git.

The overview includes two sets of four charts: Session throughput for 8/16/32/64 B messages, and post-send business cost divided by the normal 13 ns reference. Both read saved formal results without running benchmarks again.

The current business experiment is [business performance after continuous sending](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_from_empty.html). Every batch starts with zero pending messages and reports post-send cost relative to normal execution. [Procedure and reproduction commands ](https://xiyang1314.github.io/TwoBufQueue/benchmarks/guides/BURST_CACHE.html), all parameters and raw samples are included. The [earlier prepared-backlog report](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_cache.html) and figures remain distinct historical evidence with a different starting state.

To validate changes in this canonical repository:

```sh
python3 tools/refresh_release_manifest.py
python3 tools/verify_release.py
```

`tools/export_release.py DESTINATION` creates or updates a separate copy from the source tree in which it is run. Do not use an older development tree to overwrite local changes here.

The exporter follows local links, copies their targets and checks SHA-256 hashes. `release-manifest.json` lists each published file, size and hash. Measurement JSON/JSONL/CSV over 1 MiB is losslessly compressed in the destination, and published links are adjusted. `compression-manifest.json` records both hashes; `tools/unpack_performance_data.py` restores original paths and refuses to replace differing files. Recorded measurements, metadata and source archives are not rewritten. The exporter does not copy `.git`, create commits or publish anything. Differing destination files require `--update`. Extra destination files are never deleted.

[Source browser](https://xiyang1314.github.io/TwoBufQueue/docs/code/index.html) · [English HTML overview](https://xiyang1314.github.io/TwoBufQueue/README.html) · [Chinese HTML overview](https://xiyang1314.github.io/TwoBufQueue/README.zh-CN.html) · [GitHub publishing steps](https://xiyang1314.github.io/TwoBufQueue/docs/development/PUBLISHING.html) · [Report index](https://xiyang1314.github.io/TwoBufQueue/docs/reports/index.html).

The current supported comparison entry is `BRQ_BUILD_SPSC_BENCHMARKS=ON`. The old `BRQ_BUILD_BENCHMARKS` and `BRQ_BUILD_COMPARISON` APIs are deprecated and produce a clear configuration error. Their historical source is preserved in the development directory and excluded from the current release copy.

Measured-source archives and historical absolute command paths are reproduction evidence; do not rewrite them as current paths. Use the portable commands in the [benchmark guide](https://xiyang1314.github.io/TwoBufQueue/docs/reports/PERFORMANCE.html) for a new measurement. Copy-constructor sensitivity controls are retained. This publication can configure and test independently of uncopied development files.
