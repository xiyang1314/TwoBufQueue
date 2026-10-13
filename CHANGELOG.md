# Changelog

## 0.1.0 — prepared for the first public release

- C++20 producer-owned source buffers, a batch-copy worker, and an independent record callback thread.
- Independent source/output pool limits, bounded backpressure policies, explicit flush/finish and drain-on-stop.
- Single- and multi-producer read paths, optional current-block scanning, fixed-size records and sequence Writers.
- Single-block WriteSession for loops, plus typed synchronous callbacks on the producer thread.
- CMake installation and relocatable package support, runnable examples, and Linux/Windows/macOS CI configurations.
- Reproducible SPSC comparisons, block-size searches, and a continuous-batch business experiment starting each batch with zero backlog; all initialization parameters and measured data are retained.
- Business reports show post-send p50 and the ratio to normal business execution, with collapsible parameters and environment details. The earlier prepared-backlog experiment remains available as a separate measurement.

This file describes the prepared source release. A version entry does not imply that a GitHub release, remote CI run or tag already exists. Measurements identify their own hardware, compiler, configuration and metric; group-average quantiles are not individual-message latency.
