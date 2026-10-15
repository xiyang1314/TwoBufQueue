# Changelog

## 0.6.1 — 2026-10-15

- Both source archive formats now include separate `en/` and `zh_cn/` reading directories, with one shared C++ source tree.
- Language directories contain the existing introduction, API, examples, reports and source-reading pages, with local links rewritten for offline use.
- `START_HERE.html` opens the language selector. GitHub Releases provide only the automatic Source code ZIP and tar.gz downloads.
- Simplify the local archive builder to produce two bilingual archives; preserve the original v0.6.0 tag and historical measurements.

## 0.6.0 — 2026-10-15

- C++20 producer-owned source buffers, a batch-copy worker, and an independent record callback thread.
- Independent source/output pool limits, bounded backpressure policies, explicit flush/finish and drain-on-stop.
- Single- and multi-producer read paths, optional current-block scanning, fixed-size records and sequence Writers.
- Single-block WriteSession for loops, plus typed synchronous callbacks on the producer thread.
- CMake installation and relocatable package support, runnable examples, and Linux/Windows/macOS CI configurations.
- Pinned SPSC comparison headers and original licenses bundled under `third_party/spsc/` for offline benchmark builds.
- Reproducible SPSC comparisons, block-size searches, and a continuous-batch business experiment starting each batch with zero backlog; all initialization parameters and measured data are retained.
- Business reports show post-send p50 and the ratio to normal business execution, with collapsible parameters and environment details. The earlier prepared-backlog experiment remains available as a separate measurement.

This is the first public source release. Public APIs may evolve during the 0.x series. Measurements identify their own hardware, compiler, configuration and metric; group-average quantiles are not individual-message latency.
