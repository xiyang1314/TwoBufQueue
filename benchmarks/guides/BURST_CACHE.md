# Business performance after a continuous message batch

[English](https://xiyang1314.github.io/TwoBufQueue/benchmarks/guides/BURST_CACHE.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/benchmarks/guides/BURST_CACHE.html)

The [current report](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_from_empty.html) compares subsequent producer business work after continuous message bursts. Its batches start empty. The earlier prepared-backlog experiment remains a separately labelled historical test; its approximately 200 MiB prefill does not apply to the current report.

## 1. Reproduce the current report in one run

From the repository root:

```sh
python3 benchmarks/burst/run_burst_empty.py --profile docs/performance_data/burst_from_empty_20261013/selected_config.json
```

The runner uses saved initialization settings, verifies pinned dependencies and builds Release. It does not recalibrate callbacks or reselect the business working set. New samples go into a timestamped directory; the default report is `docs/reports/burst/performance_burst_from_empty.html`.

The sequence is **normal business execution → send a continuous batch → immediately execute the same business function**. Before every batch, the queue drains so starting pending bytes are 0. Consumers and copy workers continue running. Draining after measurement is outside timing. The settings are `prefill=false` and `drain_before_burst=true`; there is no 80% prepared backlog.

| Parameter | Current workload |
|---|---|
| Message length | 32 B |
| Business working set | 48 KiB |
| Callback work | 64 calculation iterations per record |
| TwoBufQueue layer-1 pool | 16 KiB total |
| Output/native storage | 256 MiB |
| Batch sizes | 2, 4, 8, 16 MiB |
| Independent runs | 3 |
| Windows per size/run | 2,500 |
| Immediate business groups per window | 4, with 16 calls per group |
| Samples per size/run | 10,000 group means |

Compute post-send p50 independently per run, then average three runs. The displayed ratio is post-send business cost divided by the normal reference. This host's reference is rounded to 13 ns/call, so the report uses 13 as the common denominator. The precise no-queue control and p99/p999 remain in the complete results. Parameters, environment and reproduction details are folded below the performance table.

All measured messages are checked for complete 32 B payload, FIFO and count. Every window must start empty and have a matching business checksum. Allocation, buffer prefaulting, business warmup and draining are excluded from business timing. Message preparation, sends and retries occur in the sending stage; the displayed table measures business execution afterward.

Use `--executable` for an existing binary, `--deps` for pinned dependencies and `--compiler` for an explicit compiler. Resume with the same `--output` and `--resume`; source, binary and profile hashes must match. Reproduction fixes the procedure and parameters, while hardware and scheduling can still change the readings.

Render saved data without measuring again:

```sh
python3 benchmarks/burst/run_burst_empty.py --render-only --output docs/performance_data/burst_from_empty_20261013 --report docs/reports/burst/performance_burst_from_empty.html
node tools/build_readme_html.mjs
```

The documentation generator localizes the public reading pages from the same saved results. The original JSON and measured-source archives remain unchanged. `python3 tools/plot_burst_release.py` generates the overview figure from saved results without rerunning the benchmark.

Turning off prefill alone does not prevent backlog from carrying into later windows. Use `drain_before_burst=true` as well when requiring an empty start.

## 2. Historical prepared-backlog experiment

```sh
python3 benchmarks/burst/run_burst_showcase.py
```

This verifies dependencies, builds Release, calibrates callbacks, selects a shared 24/48/96 KiB business working set and collects complete samples in one run. The historical default report is `docs/reports/burst/performance_burst_cache.html`.

Calibration checks 16/32/64/128/256 callback iterations and chooses the smallest setting that keeps at least 75% backlog for every queue at a 16 MiB burst. Consumers are active, without artificial pauses. Source storage is 16 KiB; output/native storage is 256 MiB. Historical batches include 1, 2, 4, 8 and 16 MiB.

Formal sampling directly collects at least 10,000 immediate business-group samples per size/run, over three independent runs. After each send window, time the first 64 business calls in groups of 16; repeat 2,500 windows. Use nearest-rank p50/p99/p999 per run and arithmetic means across runs. These are group quantiles, not individual-call tails or long-term recovery measurements. Keep all samples, hashes, checks and repeated values. Scheduling and finite tail sample counts constrain interpretation.

Replay a saved selection with:

```sh
python3 benchmarks/burst/run_burst_showcase.py --replay docs/performance_data/MY_RUN/selected_config.json
```

The saved compiler, optimization flags and settings are reused without reselecting the scene. Supply `--compiler` explicitly if using a different compiler. `--resume` checks arguments and executable hashes; `--render-only` requires complete saved results. Configurable controls include `--business-kib`, `--bursts-mib`, `--runs`, `--selection-repeats`, `--samples-per-run`, `--callback-candidates` and fixed `--callback-iterations`.

## 3. Independent diagnostic runner

`test_burst_cache` uses the same 32 B message and callback computation across transports, with independent TwoBufQueue copy/callback threads. Business work follows a shuffled dependency chain and updates 8 nodes per call through the same non-inlined function. The diagnostic can vary source-pool bytes, burst size, workset, timing groups and backlog targets. Small samples must not be promoted into formal p999 conclusions.

```sh
python3 benchmarks/spsc/bootstrap_spsc.py
cmake -S . -B build-burst -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_SPSC_BENCHMARKS=ON -DBRQ_ENABLE_POOL_DEPTH_STATS=OFF
cmake --build build-burst --target test_burst_cache --parallel
build-burst/test_burst_cache --transport two_buf_queue --source_pool_bytes 16384 --burst_bytes 4194304 --callback_iterations 256 --repeats 1
```

`Config::business_bytes` / `--business_bytes` controls the shared business working set. The recorded 24/48/96 KiB comparison selected 48 KiB for this host. Do not choose a different business load for each queue. Producer data-pool capacity is not L1 occupancy or process RSS; report source/output budgets, metadata and process memory separately. High QoS on macOS does not prevent core migration; supported platforms can request producer affinity.

## 4. Tail and instruction diagnostics

The saved 1 MiB tail experiments have separate runners and instruction-counter evidence. They are historical diagnostics rather than substitutes for the complete current batch/business report. Hardware counters require the platform's own permissions. The Chinese companion guide retains all original diagnostic commands, sample definitions and limitations.
