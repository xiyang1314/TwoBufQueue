# Third-party queues for benchmarks

[English](https://xiyang1314.github.io/TwoBufQueue/third_party/spsc/README.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/third_party/spsc/README.zh-CN.html)

These are the pinned upstream headers and licenses required by the optional SPSC comparisons. They are included in the repository and source/language packages, so building the benchmarks does not require downloading dependencies. The regular TwoBufQueue library does not depend on these queues.

## 1. Included versions

| Upstream project | Version | Fixed commit | License |
|---|---|---|---|
| [cameron314/readerwriterqueue](https://github.com/cameron314/readerwriterqueue) | v1.0.6 | `8e7627d18c2108aca178888d88514179899a044f` | BSD-2-Clause; embedded zlib |
| [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue) | commit | `1053918dbd251fbff69b24ef27fa5d51c29ec2af` | MIT |
| [max0x7ba/atomic_queue](https://github.com/max0x7ba/atomic_queue) | commit | `74fb62f86e6c3b0db824932a5c5e4bfb0125e2cc` | MIT |

This is the subset needed by the benchmarks, rather than full upstream repository copies. Original headers, copyright notices and license files are retained without changes. ReaderWriterQueue also contains the zlib-licensed semaphore implementation within `atomicops.h`.

The authoritative file list, source URLs and SHA-256 values are in `benchmarks/spsc/spsc_dependencies.json`. Neither the upstream revisions nor the benchmark workload changed when these files were bundled.

## 2. Build without downloading

From the repository root:

```sh
python3 benchmarks/spsc/bootstrap_spsc.py --check-only
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBRQ_BUILD_SPSC_BENCHMARKS=ON
cmake --build build-bench --parallel
ctest --test-dir build-bench --output-on-failure -L performance_harness
```

CMake defaults `BRQ_SPSC_DEPS` to `third_party/spsc/` and checks every listed header and license against its pinned hash. An explicit `-DBRQ_SPSC_DEPS=/absolute/path/to/deps` still selects another directory, with the same verification.

## 3. Restore or update dependencies

If a file was removed, `python3 benchmarks/spsc/bootstrap_spsc.py` can download missing files from the fixed commits. Existing files with differing hashes are rejected rather than overwritten. `--check-only` never accesses the network.

To deliberately change a dependency version, update its commit, URLs and hashes in the dependency manifest, replace the corresponding upstream files and retain their licenses, then rebuild and validate all adapters. Recorded historical benchmark results are not rewritten by an update.

See [benchmark entry points](https://xiyang1314.github.io/TwoBufQueue/benchmarks/README.html) for experiment choices and reproduction commands.
