# Examples

[English](https://xiyang1314.github.io/TwoBufQueue/examples/README.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/examples/README.zh-CN.html)

Start from the repository root. Run [01_basic.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/01_basic.cpp.html), then choose a route below. Numbers indicate the reading order within a directory; you do not need to run every example in sequence.

- `two_buf_queue/`: the complete Writer → reader → user-callback flow. Start here to use TwoBufQueue in an application.
- `components/`: standalone WarmPool, SpscLiveHandoffQueue and HandoffQueue usage. Use these when composing lower-level components yourself.

## 1. Start with the smallest example

[01_basic.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/01_basic.cpp.html) creates a Writer and TwoBufQueue, writes messages, receives callbacks, then calls `writer.finish()` and `queue.stop()`. Writer runs in the application's producer thread; the default asynchronous queue reads and invokes callbacks in background workers. Then choose a topic below.

## 2. Use the complete TwoBufQueue

| Example | Read it when | What it demonstrates |
|---|---|---|
| [02_batch_callback.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/02_batch_callback.cpp.html) | One callback should receive grouped data | TwoBufQueueBatch passes a DataBatch; the example splits each frame into records itself. |
| [03_multi_thread_writer.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/03_multi_thread_writer.cpp.html) | Multiple producer threads write | One Writer per thread, distinct thread_id values, and multiple layer-1 copy threads. |
| [04_layer1_callback.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/04_layer1_callback.cpp.html) | Process source blocks directly | Implement ReadWriterDataCallback and give it to ReadWorker; read completed/current blocks without output copying. |
| [05_seq_writer.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/05_seq_writer.cpp.html) | Records need sequence numbers | Independent SeqWriters share an atomic counter; fetch_add assigns sequence numbers and each Writer's order is verified. |
| [06_sync_writer.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/06_sync_writer.cpp.html) | Call back in the producer thread | Use the same callback shape as 01 and add SyncContext<decltype(rcvCallback)> to Writer; the callback completes before write returns. |
| [07_quick_write_muti_msg.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/07_quick_write_muti_msg.cpp.html) | Write many records consecutively | Single-block Session, nested write/acquisition loops, and explicit optional flush. |
| [08_all_parameters.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/08_all_parameters.cpp.html) | Tune a working configuration | Every Writer template argument, source pool/backpressure setting, reader setting and TwoBufQueue constructor option. |
| [09_async_logger.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/two_buf_queue/09_async_logger.cpp.html) | Send application logs to background file I/O | Independent copying/callbacks, bounded pools, Block/Drop pressure, flush, draining shutdown and complete file verification. |

Suggested route: `01 → 02` for individual-record and grouped callbacks. Read `03` for concurrency or `04` for direct source access. `05` and `08` cover sequence numbers and configuration. Use `07` for repeated fast writes, `06` for producer-thread callbacks, and `09` for file logging. The [logging guide](https://xiyang1314.github.io/TwoBufQueue/docs/guides/ASYNC_LOGGING.html) explains queue flush versus file flush.

## 3. Use individual components

| Example | Read it when | What it demonstrates |
|---|---|---|
| [11_warm_pool.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/11_warm_pool.cpp.html) | Reuse pooled resources | Implement reset(), borrow a Loan and return it with finish(). |
| [12_warm_pool_factory.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/12_warm_pool_factory.cpp.html) | Customize creation/destruction | Supply Factory::create() and dispose() in addition to the basic pool protocol. |
| [13_spsc_live_handoff_queue.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/13_spsc_live_handoff_queue.cpp.html) | Read a resource while production continues | Pool-backed SPSC handoff with current-resource access; try_pop removes records and the consumer retains pending work. |
| [14_spsc_live_handoff_queue_claim.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/14_spsc_live_handoff_queue_claim.cpp.html) | Keep pending records in the queue | Use try_claim/cancel_claim instead of pop to preserve an unhandled head record. |
| [15_handoff_queue.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/15_handoff_queue.cpp.html) | Let the queue run a consumer | SPSC HandoffQueue's built-in Reader invokes bool(Message&); false requests a later retry. |
| [16_handoff_queue_mpmc.cpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/components/16_handoff_queue_mpmc.cpp.html) | Multiple producers and consumers share work | MPMC mode with several producer threads and built-in Readers executing callbacks concurrently. |

Start with `11`, then `12` for custom lifetimes. Choose `13 → 14` for resources that are still being produced. Choose `15 → 16` when processing begins after send(). Examples 13/14 manage their consumer thread; HandoffQueue manages Readers in 15/16.

## 4. Build, run and understand callbacks

From the repository root:

```sh
cmake -S . -B build -DBRQ_BUILD_TESTS=OFF -DBRQ_BUILD_EXAMPLES=ON -DBRQ_BUILD_BENCHMARKS=OFF
cmake --build build --config Release --parallel
ctest --test-dir build -C Release -R '^example_' --output-on-failure
```

Build one target with, for example, `cmake --build build --config Release --target brq_component_15_handoff_queue`. On Windows with Ninja/MinGW, run `build/brq_component_15_handoff_queue.exe`; Visual Studio places it under `build/Release/`. Examples are independent programs: zero exit status means success.

Respect three boundaries: never access a resource after finish/return; borrowed DataBatch, source-block and payload pointers cannot outlive their callback; end producers before draining/stopping consumers. A false callback result means different things in different interfaces, as described in each example.

[example_support.hpp](https://xiyang1314.github.io/TwoBufQueue/docs/code/examples/example_support.hpp.html) contains shared example checks/parsing and includes TwoBufQueue.hpp; applications do not need this helper. Continue with the [WarmPool](https://xiyang1314.github.io/TwoBufQueue/docs/code/src/base_components/warm_pool/warm_pool.hpp.html), [SpscLiveHandoffQueue](https://xiyang1314.github.io/TwoBufQueue/docs/code/src/base_components/live_handoff_queue/spsc_live_handoff_queue.hpp.html) and [HandoffQueue](https://xiyang1314.github.io/TwoBufQueue/docs/code/src/base_components/handoff_queue/handoff_queue.hpp.html) headers for their complete interfaces.
