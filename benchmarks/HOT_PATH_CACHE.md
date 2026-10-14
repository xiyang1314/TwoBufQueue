# Small producer pools and background pending work

[English](HOT_PATH_CACHE.md) · [简体中文](HOT_PATH_CACHE.md)

This separate experiment runs business work while reusing a small TwoBufQueue source pool. A copy worker transfers messages into a fixed 256 MiB output pool, with an independent callback consumer. It does not replace the submission-only benchmark or modify upstream SPSC implementations.

## 1. Build and reproduce

```sh
python3 benchmarks/bootstrap_spsc.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_SPSC_BENCHMARKS=ON -DBRQ_ENABLE_POOL_DEPTH_STATS=OFF
cmake --build build --target test_hot_path_cache
build/test_hot_path_cache --self-test
python3 benchmarks/test_hot_path_cache.py --executable build/test_hot_path_cache --output docs/performance_data/hot_path_cache_local --report docs/performance_hot_path_cache_local.html
```

Dependencies are explicitly fetched and hash-checked; CMake does not automatically access the network. The recorded host uses Clang 18.1.8, Release `-O3 -DNDEBUG`, high producer QoS and no hard affinity. Supported Linux/Windows hosts can request `--producer_cpu`; copying and consumption have independent settings. Measure other platforms rather than importing these host results.

## 2. Workload and measurements

`hot_path_cache.hpp` defines Config and HotPathCacheTest(Config); the C++ program provides CLI settings and the Python driver serializes screening, confirmation, raw data and HTML. The driver fixes 32 B messages and 256 MiB background capacity and records every active parameter.

Business work uses a shuffled chain of 64 B nodes with dependent reads and updates. The same non-inlined Business::step function is used for all queues, with a separate integer-computation control. Messages contain sequence, business result and two varying fields. Validate complete reference messages separately and check FIFO, fixed fields, counts and checksums during formal runs.

Each condition sends 10 million messages in three independent shuffled runs. A round sends at most 80% of the fixed output budget (6,710,886 messages), then hands off and drains outside timing. TwoBufQueue uses ordinary write, maximum publication count and current scanning disabled. Source/output blocks are equal-sized, source pools have four blocks, and output pools always total 256 MiB. Two real pipeline warmup rounds are outside timing.

The three callback conditions are: hold the first callback until sending ends; 16 calculation iterations per record; or 0 calculation iterations. Zero removes simulated work, while required validation remains. Copy and callback threads are independent.

Throughput has no per-record timer. Latency times groups of 1,000 business iterations and divides by the count; it is not individual write or delivery latency. A partial final group is excluded from quantiles while all messages and counts remain. Optional sparse sampled diagnostics retain raw business/iteration time, including the host's approximately 41.7 ns timer quantization.

## 3. Selection and evidence

Search TwoBufQueue block sizes under the same business scene using weights 35% throughput, 30% p50, 20% p99 and 15% p999. Require neighboring support and choose the smallest source pool within 2% of the best score. Native copy variants remain disclosed and independently checked. Confirmation uses three fresh runs and one queue configuration across callback conditions.

Source-pool bytes are a capacity limit, not L1 occupancy or total RSS. TwoBufQueue's total is source plus 256 MiB output; native storage is approximately 256 MiB. Metadata and business memory are separate. Without new PMU samples, the report does not attribute speed differences directly to a specific cache mechanism.

Complete results, settings, commands, repeated values, hashes and checks remain in summary.json, results.csv, manifest.json and raw/*.json. Resume rejects changed executables or commands. The Chinese companion keeps the original full experiment notes.
