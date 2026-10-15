# Configurable SPSC benchmarks

[English](https://xiyang1314.github.io/TwoBufQueue/benchmarks/guides/TUNABLE_SPSC.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/benchmarks/guides/TUNABLE_SPSC.html)

The benchmark runs the complete asynchronous TwoBufQueue pipeline: `Writer → SingleReadWorker → layer-2 copy → output queue → per-record callback`. Controls use the pinned upstream SPSC implementations of rigtorp/SPSCQueue, moodycamel ReaderWriterQueue and atomic_queue. Every implementation transfers the complete payload by value; it does not send a pointer or add a timestamp.

Use identical successful write counts, warmup counts, message lengths and callback work across transports. Throughput is in million messages/s and net-payload GB/s. The p50/p99/p999 values normally describe **1,000-write group means**, not single-call or end-to-end latency. Tune each objective separately when exploring; published common-configuration comparisons disclose their selection weights and use one parameter set for all metrics.

## 1. Build and run

Requires Python 3.9+, CMake 3.20+ and a C++20 compiler. macOS requires 14.4+. Pinned benchmark headers and licenses are bundled in `third_party/spsc/`. The check below verifies the hashes in `spsc_dependencies.json` without network access; ordinary library builds do not depend on these queues.

```sh
python3 benchmarks/spsc/bootstrap_spsc.py --check-only
cmake -S . -B build-spsc -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_SPSC_BENCHMARKS=ON -DBRQ_BUILD_TESTS=OFF -DBRQ_BUILD_EXAMPLES=OFF -DBRQ_BUILD_FUNCTIONAL_TESTS=OFF
cmake --build build-spsc --config Release --target brq_tunable_benchmark --parallel
python3 benchmarks/spsc/tune_spsc.py ./build-spsc/brq_tunable_benchmark --profile benchmarks/profiles/spsc_quick.json --output benchmark-results/my-machine
```

Visual Studio places the binary at `build-spsc/Release/brq_tunable_benchmark.exe`; Ninja/MinGW use `build-spsc/brq_tunable_benchmark.exe`. Explicitly build Release with a multiconfiguration generator. The runner rejects unoptimized or sanitized binaries; `--allow-debug` is only for correctness checks. An offline dependency directory can be supplied with `-DBRQ_SPSC_DEPS=/absolute/path/to/deps`; CMake verifies its hashes.

## 2. Fixed-configuration ordinary write and Session runs

```sh
python3 benchmarks/spsc/run_release_benchmarks.py --binary ./build-spsc/brq_tunable_benchmark --output docs/performance_data/my-release --report-dir docs/reports/comparison
```

The default runner measures 8/16/32/64 B, 10 million successful messages per run and three runs per metric, retaining every trial. Callback iterations are 0 for 8 MiB. For 256 MiB, ordinary write calibrates the callback interval/send-time ratio toward 2–3; the selected loop count is then shared by Session and all controls at that length. If the target is not reached within the calibration budget, the runner records `target_met=false` and the actual ratio. The ratio includes dispatch, waiting and scheduling; it is not the sum of callback CPU times.

The release profile enables current-block scanning for ordinary write, with 1 ms initial/maximum scan intervals and minimum wait, publishing every 10 writes. Session retains scanning disabled and maximum publication count. Pool budgets are equal, but reader/publication settings differ, so these are not same-parameter controls. Other published modes have their own disclosed settings. See [performance evidence](https://xiyang1314.github.io/TwoBufQueue/docs/reports/PERFORMANCE.html) for exact report configurations.

The runner writes HTML, raw JSONL, complete settings, every command, source archives, source/dependency/executable hashes, compiler flags and environment information. Existing selected reports are archived. Native controls measured in the same batch are shared between ordinary-write and Session reports. They are not filled in from historical scores.

Use a new output directory for each experiment. `--resume` accepts only identical source, executable and settings, and reuses complete saved trials. Use `--write-mode normal` to update ordinary-write reports alone. To isolate a reader/publication change, `--callback-iterations 1 1 2 4` fixes the 256 MiB loop counts without recalibrating; 8 MiB remains 0. Actual ratios can then fall outside 2–3.

Small runner check, not a formal performance result:

```sh
python3 benchmarks/spsc/run_release_benchmarks.py --binary ./build-spsc/brq_tunable_benchmark --output benchmark-results/smoke --report-dir benchmark-results/smoke/reports --cache 8 --messages 10000 --repetitions 1
```

On macOS, the producer uses user_interactive QoS without hard CPU affinity; formal local runs require AC power. Other platforms keep their scheduling unchanged by default and can request `--producer-cpu N` where supported. Copy and consumer affinity are configured independently. `--timer steady` selects the portable clock; unavailable hardware information is recorded rather than guessed. `--hardware-info hardware.json` attaches separately collected host information with its hash.

## 3. Payload and copy contracts

`payload_bytes` is the net length, including a sequence number in its first 8 bytes. The benchmark uses `Writer<publish_count, false>` without a length prefix; fixed-size batch parsing invokes the same Sink once per record. Every 8-byte payload word varies with its sequence, preventing a constant tail from dominating compiler optimization.

The external Payload retains `bytes{}` initialization and ordinary `bytes(other.bytes)` full member copying. rigtorp can request a destination write-prefetch constructor through its original `try_emplace`. ReaderWriterQueue can request zeroing followed by a complete memcpy; published 16/32 B settings enable this path while 8/64 B use member copying. Extra zeroing is timed. Upstream queue headers remain unchanged. Copy choices are selected before timing and disclosed because their generated code can affect rankings independently of the queue algorithm. [Copy-control evidence](https://xiyang1314.github.io/TwoBufQueue/docs/reports/PERFORMANCE.html#copy-constructor-sensitivity).

## 4. Use the test class

Headers live under `benchmarks/tunable/` and are not part of the library hot path. Link the `brq_performance_support` CMake target for library and benchmark-dependency include paths.

```cpp
#include "tunable/performance_test.hpp"
#include <iostream>

int main() {
    namespace perf = TwoBufQueue::performance;
    perf::PerformanceConfig config;
    config.payload_bytes = 8;          // Fixed workload requirement.
    config.messages = 5'000'000;       // Successful measured writes.
    config.latency_batch_size = 1000;  // Writes per latency timing group.
    config.callback_iterations = 0;   // Simulated work per received record.
    config.source.block_size = 65536;
    config.output.output_block_size = 65536;
    config.publish_count = 4;
    config.mode = perf::MeasureMode::throughput;
    const auto result = perf::PerformanceTest(config).run();
    std::cout << result.m_messages_per_second << " M msg/s\n";
}
```

Construction validates the complete configuration. Each `run()` creates fresh queues and threads. Template dispatch occurs before timing; writes do not use virtual calls or std::function. The same xorshift callback work, compiler constraints and final checksum prevent removal of simulated work. With `callback_iterations=0`, simulated computation is skipped, while counting, ordering checks and completion notification remain. This does not remove all consumer/framework costs.

Inputs are prepared before timed sending and read/copied identically for every transport. Prepared input can exceed L1, so it is a streaming-input workload. Its bytes are accounted separately from queue storage. `wall_seconds` starts after first-round preparation and includes later preparation and draining; all preparation time is also recorded. Throughput uses accumulated send time, including retries, waits and block switching, but excludes round draining.

## 5. Parameters

`--defaults` prints every flattened parameter. Supply a value with `--field value`; C++ uses nested `source`, `output` and `output.read_config`. The complete authoritative mapping is in `parameters.hpp`, and every result records the resolved settings.

| Group | Fields | Meaning |
|---|---|---|
| Workload | `payload_bytes`, `messages`, `warmup_messages` | Net payload length, successful measured messages and separate warmup. Effective warmup aligns with natural publication and is recorded. |
| Prepared input | `prepared_input`, `variable_payload`, `prepared_input_limit_bytes` | Prepare a complete varying round in advance; input has a separate byte limit and accounting. Keep this workload fixed while tuning queues. |
| Callback/arrival | `callback_iterations`, `producer_interval_ns` | Per-record simulated work; 0 arrival interval means consecutive input, otherwise use a busy-wait cadence. |
| Source pool | `source_block_size`, `source_pool_min`, `source_pool_max`, `source_queue_capacity` | Block bytes, pool bounds and at least 2 source handoff entries. |
| Publication | `publish_count`, `write_mode` | Publication batch and ordinary/Session path. No per-message active flush; block handoff and finish retain library semantics. |
| Writer pressure | `writer_backpressure`, `writer_spin_count`, `writer_wait_timeout_ms` | Drop/Block policy, spinning and pool wait timeout. The driver retries rejected writes instead of losing messages. |
| Output blocks | `output_block_size`, `output_frame_capacity` | Block bytes and source-range descriptor capacity; output blocks must fit the source block. |
| Output capacity | `output_pool_min`, `output_pool_max`, `output_queue_capacity` | Output pool and handoff bounds. Source and output budgets are separate. |
| Reader settings | `is_scan_current`, `rescan_interval_ns`, `rescan_max_interval_ns`, `reader_spin_count` | Current-block scanning, scan intervals and spin behavior. |
| Native controls | `spsc_capacity`, `spsc_idle_spins`, `spsc_idle_sleep_ns` | Slot capacity and consumer waiting. Reserved slots/padding count toward the data budget. |
| Native copy | `rigtorp_write_prefetch`, `readerwriterqueue_copy_after_zero` | Optional external Payload construction choices, fixed before timing. |
| Scheduling | `producer_cpu`, `consumer_cpu`, `producer_qos` | Requested affinity/QoS; results record whether the platform supports it. |
| Timing | `timer`, `latency_batch_size`, `sample_every`, `max_samples` | Clock and sampling/grouping. Group quantiles are not individual-message latency. |
| Reproduction | `random_seed`, `timeout_seconds` | Deterministic payload/test inputs and the process timeout. |

Consult `brq_tunable_benchmark --help` and `--defaults` for every supported field, type and default; table groups are an overview, not a replacement for the recorded complete configuration.

## 6. Search under matching budgets

The profile fixes the workload, platform settings, output budget and round boundaries; only selected queue parameters vary. Every round's bytes derive from the fixed output budget (normally its 80% limit), not the changing source pool. Preserve these conditions when comparing searched configurations with native queues.

The search saves screening and confirmation trials, complete-payload/FIFO checks, candidates, raw results and the chosen configuration for each objective. Quantile stability and neighboring candidates matter: use supported plateaus rather than isolated lucky results. Current shared-configuration reports combine throughput/p50/p99/p999 with weights 35%/30%/20%/15%, equally across four lengths. No finite search proves a global optimum.

## 7. Read and regenerate reports

The checked-in English and Chinese report pages contain the same measurements. `node tools/build_readme_html.mjs` regenerates reading pages; it does not rerun performance tests. Original measured source, commands and raw JSON retain their historical language and bytes. The Chinese companion guide additionally preserves the complete original experiment notes and historical parameter tables.
