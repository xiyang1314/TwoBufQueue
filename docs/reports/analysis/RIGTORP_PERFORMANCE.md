# rigtorp benchmark regression

[English](https://xiyang1314.github.io/TwoBufQueue/docs/reports/analysis/RIGTORP_PERFORMANCE.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/docs/reports/analysis/RIGTORP_PERFORMANCE.zh-CN.html)

This correction changes the external benchmark adapter. The original rigtorp, ReaderWriterQueue and atomic_queue headers retain their pinned versions and original SHA-256 hashes.

The ordinary Payload copy constructor remains `bytes(other.bytes)`. A constructor tagged `DestinationWritePrefetch` hints that the destination will be written, then performs the same complete member copy once. The rigtorp adapter requests it through the original `try_emplace` API. It adds no zeroing, removes no payload bytes and does not change release/acquire publication.

`PerformanceConfig::rigtorp_write_prefetch` defaults to true; use `--rigtorp_write_prefetch false` to disable it. The benchmark selects `RigtorpAdapter<Payload, true/false>` before timing, so there is no runtime switch inside the write loop. Calls for ReaderWriterQueue, atomic_queue and TwoBufQueue are unchanged by this correction.

## 1. Same-executable control

The control uses one optimized executable, the same fully varying payload and 10 million messages per run. Throughput and quantiles of 1,000-write group means each have five runs. Both switch states pass full-payload/FIFO validation. Callback iterations are 0 for 8 MiB and fixed at 1/1/3/5 for 8/16/32/64 B at 256 MiB, matching the earlier calibration. The four formal reports recalibrate independently and retain every run. The producer uses high QoS without hard affinity, on AC power.

| Capacity | Message | Off: M messages/s | On: M messages/s | Off: p50 ns | On: p50 ns |
|---|---|---:|---:|---:|---:|
| 8 MiB | 8 B | 44.51 | 345.24 | 22.083 | 2.758 |
| 8 MiB | 16 B | 43.48 | 349.74 | 23.192 | 2.800 |
| 8 MiB | 32 B | 43.90 | 303.25 | 22.625 | 3.142 |
| 8 MiB | 64 B | 50.90 | 221.58 | 19.242 | 4.283 |
| 256 MiB | 8 B | 47.97 | 313.66 | 19.475 | 3.067 |
| 256 MiB | 16 B | 44.70 | 266.82 | 21.492 | 3.783 |
| 256 MiB | 32 B | 39.36 | 162.59 | 25.683 | 6.000 |
| 256 MiB | 64 B | 39.84 | 98.23 | 24.717 | 9.883 |

All 160 formal measurements and 16 complete validations passed. [Summary](../../performance_data/rigtorp_prefetch_20261011/selective-control/summary.json) · [Raw records](../../performance_data/rigtorp_prefetch_20261011/selective-control/raw.jsonl) · [Source and reproduction scripts](../../performance_data/rigtorp_prefetch_20261011/selective-control/source.tar.gz).

## 2. Code generation and limits

In the saved 16 B push assembly, both paths use one `ldr q0`, one `str q0` and the same `stlur` publication. The enabled path adds `prfm pstl1keep` before the copy; it adds no zeroing stores. [Assembly](https://xiyang1314.github.io/TwoBufQueue/docs/code/docs/performance_data/rigtorp_prefetch_20261011/push_codegen.s.html).

`__builtin_prefetch(destination, 1, 3)` requests write prefetch with high temporal locality; it does not write the payload. This hint does not guarantee a speedup on every platform. See the [GCC documentation](https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html#index-__builtin_prefetch). Compilers without this builtin use a normal complete copy.

The control confirms that destination write prefetch removes the regression in this call path on the measured host, without restoring the extra zeroing of the constructor-body memcpy path. Hardware counters have not confirmed the specific M3 pipeline or cache-ownership mechanism, and these results do not establish a universal queue or platform ranking.

Placing the queue object at offsets 0 and 64 modulo 128 did not improve the regression, so no alignment change was adopted. [Placement control](../../performance_data/rigtorp_prefetch_20261011/placement-control/summary.json).

An earlier variant added the hint to the ordinary Payload copy for every adapter. rigtorp improved, but 8 B ReaderWriterQueue throughput fell from about 766 to 481 M messages/s, so that variant was rejected. [Shared-hint control](../../performance_data/rigtorp_prefetch_20261011/fix-control/summary.json). The adopted correction requests the hint only in the rigtorp adapter. Later ReaderWriterQueue 16/32 B copy changes are disclosed separately in the [benchmark methodology](https://xiyang1314.github.io/TwoBufQueue/docs/reports/PERFORMANCE.html); they are not part of this prefetch-switch control.

## 3. Subsequent complete measurements

The four reports below were remeasured at commit `f698f25`: all 400 independent formal measurements, 40 full-payload/FIFO validations and 45 callback calibration runs completed. For rigtorp at 8 MiB / 16 B, the five-run means are 317.500 M messages/s and 3.017 ns group-average p50. At 256 MiB, callback iterations are fixed to 1/1/2/4 for 8/16/32/64 B, with calibration ratios 3.09/2.17/1.95/2.38; 8 B and 32 B slightly miss the target. That run removes forced inlining from benchmark lambdas and does not change rigtorp prefetch or upstream headers. Ordinary write and Session share freshly measured native results for the same capacity. The earlier switch control remains above; do not attribute every difference between runs to prefetch.

[8 MiB ordinary write](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_8m_zero.html) · [8 MiB Session](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_8m_zero_session.html) · [256 MiB ordinary write](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_256m_callback.html) · [256 MiB Session](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_256m_callback_session.html) · [All raw records](../../performance_data/release_20261011_inline_fixed_110024/raw.jsonl.gz) · [Run manifest](../../performance_data/release_20261011_inline_fixed_110024/manifest.json).
