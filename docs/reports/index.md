[English](https://xiyang1314.github.io/TwoBufQueue/docs/reports/index.html) · [中文](https://xiyang1314.github.io/TwoBufQueue/docs/reports/index.zh-CN.html)

# TwoBufQueue

Small producer buffers. Batch copying. Independent callbacks. · Small producer buffers, with consumer capacity for pending work.

[Project overview](https://xiyang1314.github.io/TwoBufQueue/README.html) · [Chinese overview](https://xiyang1314.github.io/TwoBufQueue/README.html)

C++20 Apache-2.0 Measured on Apple M3 Parameters and raw evidence included

[Continuous sending · each batch starts empty

## How does a continuous message batch affect business performance?

Normal business → send a batch → immediate business · 2–16 MiB

**Open report →**](burst/performance_burst_from_empty.html) · [WriteSession

## Single-block write loop

256 MiB · empty callback · 3 independent runs

**Open report →**](comparison/performance_256m_zero_session_plateau.html) · [Publication at block handoff

## Ordinary write: maximum publication count

256 MiB · empty callback · 3 independent runs

**Open report →**](comparison/performance_256m_zero_publish_max_plateau.html) · [Publication every record

## Ordinary write: publish every record

256 MiB · empty callback · 3 independent runs

**Open report →**](comparison/performance_256m_zero_publish1_plateau.html)

## Business performance after continuous sending

![Business group-average p50 after continuous 2,4,8,16 MiB batches, each starting empty, with normal business at 13 ns per call](../assets/burst_business_cost.svg)

Normal business → send a continuous message batch → immediately run the same business. Every batch starts with zero pending messages; consumers stay active and draining happens outside timing. Same 48 KiB business data set, 32 B messages and 64 callback iterations. TwoBufQueue uses 16 KiB of source buffers and 256 MiB of output storage; native SPSC queues use 256 MiB of slots.

Normal business costs about 13 ns/call (dashed line). After 16 MiB: TwoBufQueue 18.2 ns/call (1.40×), ReaderWriterQueue 58.2 ns, rigtorp 42.5 ns, atomic_queue 99.8 ns. At 2 MiB, TwoBufQueue and rigtorp tie at 15.6 ns/call.

p50 of 16-call business group means, averaged across three independent runs. High producer QoS; no hard CPU affinity. [Full results, ratios and initialization parameters](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_from_empty.html).

## Read the numbers correctly

Write reports measure producer submission cost in groups of 1,000 writes. The batch/business reports measure business execution in groups of 16 calls. Neither is individual-message or end-to-end latency. Thread counts, source/output pools and upstream payload construction choices are stated in each report; rankings apply to the recorded setup.

[Methodology and reproduction](https://xiyang1314.github.io/TwoBufQueue/docs/reports/PERFORMANCE.html) · [Publication contents](https://xiyang1314.github.io/TwoBufQueue/docs/development/RELEASE_CONTENTS.html) · [Publishing steps](https://xiyang1314.github.io/TwoBufQueue/docs/development/PUBLISHING.html)

### Additional measurements and earlier experiments (Chinese records)



- [performance 256m callback](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_256m_callback.html)
- [performance 256m callback session](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_256m_callback_session.html)
- [performance 8m 16b 32b 64b tuned](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_8m_16b_32b_64b_tuned.html)
- [performance 8m 16b 32b tuned](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_8m_16b_32b_tuned.html)
- [performance 8m 32b latency](https://xiyang1314.github.io/TwoBufQueue/docs/reports/analysis/performance_8m_32b_latency.html)
- [performance 8m zero](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_8m_zero.html)
- [performance 8m zero session](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_8m_zero_session.html)
- [performance burst business sizes](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_business_sizes.html)
- [performance burst tail 1m](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_tail_1m.html)
- [performance burst tail instruction 1m](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_tail_instruction_1m.html)
- [performance hot path cache](https://xiyang1314.github.io/TwoBufQueue/docs/reports/analysis/performance_hot_path_cache.html)
- [performance optimal ordinary readback reader2g zero](https://xiyang1314.github.io/TwoBufQueue/docs/reports/analysis/performance_optimal_ordinary_readback_reader2g_zero.html)
- [performance optimal three modes reader2g zero](https://xiyang1314.github.io/TwoBufQueue/docs/reports/analysis/performance_optimal_three_modes_reader2g_zero.html)
- [test per write block size only writer publish1](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_only_writer_publish1.html)
- [test per write block size only writer publish max](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_only_writer_publish_max.html)
- [test per write block size only writer reader80 3runs publish1](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_only_writer_reader80_3runs_publish1.html)
- [test per write block size only writer reader80 3runs publish max](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_only_writer_reader80_3runs_publish_max.html)
- [test per write block size readback fixed reader 3runs](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_write_block_size_readback_fixed_reader_3runs.html)
- [test per writer block size](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size.html)
- [test per writer block size cache hits](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size_cache_hits.html)
- [test per writer block size max comparison 3runs](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size_max_comparison_3runs.html)
- [test per writer block size no current](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size_no_current.html)
- [test per writer block size no current publish1](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size_no_current_publish1.html)
- [test per writer block size reader80 3runs max](https://xiyang1314.github.io/TwoBufQueue/docs/reports/block_size/test_per_writer_block_size_reader80_3runs_max.html)

Prepared reports retain their original measurement dates. Packaging does not rerun or replace measurements.
