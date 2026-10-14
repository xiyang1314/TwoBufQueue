# TwoBufQueue

**TwoBufQueue is a C++ producer–consumer message queue component for passing messages and data between threads.**

Its two-layer buffer design prioritizes producer hot-path performance. Batch publication and transfer let producers use small buffers while consumers use larger buffers for pending work, reducing the effect of message transfer on business execution.

[Quick overview](README.md) · [Documentation index](docs/index.md) · [简体中文](README_DETAIL.zh-CN.md) · [API guide](docs/guides/API.md) · [Benchmarks](docs/reports/PERFORMANCE.md) · [Examples](examples/README.md) · [Source browser](docs/code/index.md)

**Contents**

- [1. How fast is it?](#1-how-fast-is-it)
  - [1.1 How fast can messages be submitted?](#11-how-fast-can-messages-be-submitted)
  - [1.2 After a continuous message batch, how fast is business execution?](#12-after-a-continuous-message-batch-how-fast-is-business-execution)
  - [1.3 Earlier matched producer-write experiment](#13-earlier-matched-producer-write-experiment)
- [2. Problems and solutions](#2-problems-and-solutions)
  - [2.1 Why can business execution slow down several times even without backpressure?](#21-why-can-business-execution-slow-down-several-times-even-without-backpressure)
  - [2.2 How much faster is batch publication and handoff than per-record handoff?](#22-how-much-faster-is-batch-publication-and-handoff-than-per-record-handoff)
  - [2.3 When message buffers are full, must the producer wait?](#23-when-message-buffers-are-full-must-the-producer-wait)
  - [2.4 Small buffers cause backpressure; large buffers waste memory. What should you do?](#24-small-buffers-cause-backpressure-large-buffers-waste-memory-what-should-you-do)
  - [2.5 How can large messages be sent more efficiently?](#25-how-can-large-messages-be-sent-more-efficiently)
  - [2.6 Does reusing the most recently returned free block matter?](#26-does-reusing-the-most-recently-returned-free-block-matter)
- [3. Comparison with typical single-layer queues](#3-comparison-with-typical-single-layer-queues)
  - [3.1 SPSC: one producer and one consumer](#31-spsc-one-producer-and-one-consumer)
  - [3.2 MPMC: multiple producers and consumers](#32-mpmc-multiple-producers-and-consumers)
  - [3.3 Design highlights](#33-design-highlights)
- [4. Use cases](#4-use-cases)
  - [4.1 Where the design helps](#41-where-the-design-helps)
  - [4.2 Where the tradeoffs matter](#42-where-the-tradeoffs-matter)
- [5. Getting started](#5-getting-started)
  - [5.1 Minimal example](#51-minimal-example)
  - [5.2 Choose a starting point](#52-choose-a-starting-point)
  - [5.3 Build and integrate](#53-build-and-integrate)
  - [5.4 Usage boundaries](#54-usage-boundaries)
- [6. Roadmap](#6-roadmap)

---

## 1. How fast is it?

They say men shouldn't be too fast. But as a programmer, the code you write should be as fast as possible.

### 1.1 How fast can messages be submitted?

To pursue extreme performance, TwoBufQueue provides a single-block `WriteSession` for fast consecutive writes into the current block.

The four charts show **producer submission throughput from the Session comparison, in million messages/s; higher is better**. TwoBufQueue uses one common configuration across all four message sizes. Native queues use their measured configurations with upstream code unchanged.

![Session submission throughput for 8, 16, 32 and 64 B messages on Apple M3](docs/assets/session_write_throughput.svg)

Only producer sending is timed, including block acquisition, switching and waits during sending. Round draining is outside timing. Consumers run concurrently, including layer-2 copying. Callback loop count is 0; native slot storage and TwoBufQueue output storage are 256 MiB. This Session configuration also uses 256 MiB of producer storage, a separate experiment from the 16 KiB business-impact case below.

| Submission pattern | Full results |
|---|---|
| Write many messages within one block | [WriteSession](docs/reports/comparison/performance_256m_zero_session_plateau.md) |
| Ordinary write with publication at block handoff | [Batch publication](docs/reports/comparison/performance_256m_zero_publish_max_plateau.md) |
| Ordinary write with per-record progress publication | [Publish every record](docs/reports/comparison/performance_256m_zero_publish1_plateau.md) |

Reports include p50, p99, p999 and backpressure. Write-cost quantiles use **1,000-write group averages**, not individual-call or end-to-end latency. Submission modes are tuned separately. [Benchmark methodology](docs/reports/PERFORMANCE.md) includes workloads, parameters, three-run data and reproduction commands.

### 1.2 After a continuous message batch, how fast is business execution?

Fast submission matters. So does the work that follows it. Robotics control, high-frequency trading and other low-latency systems place demanding requirements on the hot path. After sending a batch of messages, how much can the time to execute the same code differ?

The measurements show that performance can differ by several times. The experiment gives every queue the same sequence: **run the normal business function → send a continuous batch → immediately run the same function again**, then measures the effect on business execution.

The values below mean **post-send business execution time ÷ normal business execution time**. This section shows the results; the next chapter explains the reasons.

![Post-send business cost divided by the normal 13 ns reference, for 2, 4, 8 and 16 MiB bursts](docs/assets/burst_business_ratio.svg)

**Normal business execution costs about 13 ns/call. After a 16 MiB batch: TwoBufQueue 18.2 ns/call, ReaderWriterQueue 58.2 ns/call, rigtorp 42.5 ns/call, atomic_queue 99.8 ns/call.** TwoBufQueue uses only 16 KiB of producer message storage and a 256 MiB consumer pool; the other queues each have 256 MiB of slot storage.

### 1.3 Earlier matched producer-write experiment

This chart compares the **same TwoBufQueue parameters**, ordinary `write` and single-block Session. Apple M3 / macOS 14.4.1 / Clang 18.1.8; 10 million messages per run, five runs; callback loop count 0; each layer's payload pool 8 MiB (16 MiB total). High QoS, **no hard CPU affinity**. p50 is the quantile of **1,000-write group averages**, not individual-call or end-to-end latency. Send timing excludes round draining, but includes block acquisition and switching. [Chart source and full measurement conditions](docs/reports/PERFORMANCE.md).

| Condition | Ordinary write | WriteSession |
|---|---|---|
| 8 MiB per layer, callback loop count 0 | [Historical report (Chinese)](docs/reports/comparison/performance_8m_zero.md) | [Historical report (Chinese)](docs/reports/comparison/performance_8m_zero_session.md) |
| 256 MiB per layer, calibrated callback work | [Historical report (Chinese)](docs/reports/comparison/performance_256m_callback.md) | [Historical report (Chinese)](docs/reports/comparison/performance_256m_callback_session.md) |

Reports also include ReaderWriterQueue, rigtorp/SPSCQueue and atomic_queue. All measured trials, parameters and commands are retained. An independent copy-constructor A/B shows large changes in some native queues even with identical payloads and valid data; these differences cannot all be attributed to queue algorithms. See [benchmark methodology](docs/reports/PERFORMANCE.md) before interpreting the comparison.

## 2. Problems and solutions

Submission cost is one part of hot-path performance. These six practical questions explain what TwoBufQueue does, the benefits it aims for and its current boundaries.

### 2.1 Why can business execution slow down several times even without backpressure?

It is easy to assume that a queue with enough capacity leaves business performance unaffected as long as the producer never waits.

Even without backpressure, business execution can slow down substantially after the producer sends a continuous batch. Sending many messages touches a large amount of memory and can displace the business thread's hot data from CPU caches.

TwoBufQueue reuses a small producer message pool and leaves larger backlog storage to the consumer, aiming to reduce interference with subsequent business execution. The experiment above measures this effect: [post-send business performance](docs/reports/burst/performance_burst_from_empty.md).

### 2.2 How much faster is batch publication and handoff than per-record handoff?

How large can the gap be? The measured hot-path configurations differ by several times.

For 8 B messages on this host, **Session loops reach about 2,306.5 million messages/s, ordinary-write batch publication about 1,022 million messages/s, and per-record publication about 490 million messages/s.** Relative to per-record publication, Session reaches about **4.7×**, and ordinary-write batch publication about **2.1×**.

See [Session](docs/reports/comparison/performance_256m_zero_session_plateau.md), [ordinary-write batch publication](docs/reports/comparison/performance_256m_zero_publish_max_plateau.md), and [per-record publication](docs/reports/comparison/performance_256m_zero_publish1_plateau.md). Each mode uses its tuned configuration; these numbers compare the complete measured configurations.

### 2.3 When message buffers are full, must the producer wait?

A brief consumer slowdown does not force a typical single-layer queue to stop accepting messages while space remains. The question arises once its capacity limit is reached: the producer may wait or reject the write according to its policy.

TwoBufQueue currently uses independently configured buffers and background copying to absorb brief backlogs and return producer blocks sooner. Capacity is finite; a full consumer pool or insufficient copy progress can still cause backpressure.

**Planned solution: serialize layer-2 buffers to files and read them back.** As layer-2 memory approaches its limit, save some pending messages to files and later read them back for consumption, extending backlog capacity with temporary file storage.

The two-layer structure lets this extension live in layer 2. Background workers handle file storage and readback while the producer keeps its layer-1 writing and handoff path. Available disk capacity and write speed still constrain the backlog that can be handled. **This feature is not implemented yet.** [Roadmap](#6-roadmap).

### 2.4 Small buffers cause backpressure; large buffers waste memory. What should you do?

A small capacity may not cover bursts; a large one may retain memory for peaks that happen only occasionally. The wider the gap between typical traffic and peaks, the harder the choice becomes.

TwoBufQueue can use a generous memory limit while its pool allocates blocks as needed. With a configured free-block retention limit, surplus free blocks enter the reclamation path, adjusting allocation according to the preset parameters.

Convenience construction retains free blocks by default. Configure a retention limit on a manually constructed source pool when automatic reclamation is needed. See [dynamic pool allocation and reclamation](docs/guides/API.md#61-on-demand-allocation-and-free-block-reclamation).

### 2.5 How can large messages be sent more efficiently?

Most messages may be small, with an occasional large snapshot, sample batch or data block. Even sending just one large message can touch a broad message-memory range and disturb cached business data, making subsequent work slower.

The issue here is how much memory a send touches. It resembles the interference caused by a continuous batch of small messages; large messages also raise the question of keeping the producer writing area small.

**Planned solution: split and reassemble large messages.** Transfer a large message as multiple smaller blocks through the copy stage, then reconstruct it at the consumer. **Expected benefit:** keep the producer reusing a small message-memory area even for large messages, reducing interference with the business thread's hot cache and helping business execution remain stable.

**This feature is not implemented yet.** Each complete record currently must fit one source block. The 32 B batch experiment above does not measure large-message splitting; a dedicated experiment will follow implementation. [Roadmap](#6-roadmap).

### 2.6 Does reusing the most recently returned free block matter?

Yes. It is particularly useful for continuous batch sending, and the planned large-message splitting feature will use the same reuse strategy.

Suppose the producer pool has a **10 MiB** limit and many blocks have already been allocated. Cycling through every free block can spread batch writes over that 10 MiB area, increasing interference with the business thread's hot cache.

Taking the most recently returned block first lets production concentrate on fewer blocks when the background stage returns them promptly. This reduces the repeatedly written memory range and can reduce business-cache interference. TwoBufQueue uses this strategy.

[Pool design and implementation](src/base_components/WarmPool/warm_pool.hpp).

## 3. Comparison with typical single-layer queues

Consumer capacity and producer memory use often pull in opposite directions: the consumer needs room for pending work, while the producer benefits from reusing a small message area.

### 3.1 SPSC: one producer and one consumer

A typical single-layer queue uses shared message storage for the producer and consumer. Pending messages remain in that storage.

```mermaid
flowchart LR
    A[Producer thread] --> B[Message queue buffer]
    B --> C[Consumer thread]
    classDef producer fill:#f0f5f8,stroke:#d8e4ec,color:#315b73;
    classDef shared fill:#edf0f8,stroke:#d6ddec,color:#315b73;
    classDef consumer fill:#f0f5f8,stroke:#d8e4ec,color:#315b73;
    class A producer;
    class B shared;
    class C consumer;
```

TwoBufQueue configures a small, reusable source pool separately from a larger output pool. A copy worker transfers batches so source blocks can return to the producer sooner.

```mermaid
flowchart LR
    A[Producer thread] --> B[Small producer buffer]
    B --> C[Copy worker]
    C --> D[Large consumer buffer]
    D --> E[Consumer thread]
    classDef producer fill:#f0f5f8,stroke:#d8e4ec,color:#315b73;
    classDef source fill:#e7f5ee,stroke:#b6dace,color:#315b73;
    classDef copy fill:#f0f5f8,stroke:#d8e4ec,color:#315b73;
    classDef output fill:#eaf1fa,stroke:#c3d6e9,color:#315b73;
    classDef consumer fill:#f0f5f8,stroke:#d8e4ec,color:#315b73;
    class A producer;
    class B source;
    class C copy;
    class D output;
    class E consumer;
```

### 3.2 MPMC: multiple producers and consumers

Each producer has its own Writer and source pool. The default is one consumer thread; SPMC/MPMC output modes support multiple consumer threads. Callbacks can copy data into application-owned storage and submit it to an application thread pool.

This example uses two producers, two copy threads and two consumers. Each producer has its own Writer and small message buffer. Copy threads hand data over to shared consumer storage, where consumers claim different batches for parallel processing.

```mermaid
flowchart LR
    A[Producer thread 1] --> B[Small producer buffer 1]
    B --> C[Copy thread 1]
    D[Producer thread 2] --> E[Small producer buffer 2]
    E --> F[Copy thread 2]
    C --> G[Shared large consumer buffer]
    F --> G
    G --> H[Consumer thread 1]
    G --> I[Consumer thread 2]
    classDef producer fill:#f0f5f8,stroke:#d8e4ec,color:#315b73;
    classDef source fill:#e7f5ee,stroke:#b6dace,color:#315b73;
    classDef copy fill:#f0f5f8,stroke:#d8e4ec,color:#315b73;
    classDef output fill:#eaf1fa,stroke:#c3d6e9,color:#315b73;
    classDef consumer fill:#f0f5f8,stroke:#d8e4ec,color:#315b73;
    class A,D producer;
    class B,E source;
    class C,F copy;
    class G output;
    class H,I consumer;
```

Consumers share the work; each message is not broadcast to every consumer. Thread counts are configurable. See [thread and output-mode configuration](docs/guides/API.md#9-multiple-producers-and-consumers).

### 3.3 Design highlights

Both TwoBufQueue diagrams show the two-layer asynchronous path. An optional direct read path skips layer-2 copying; a non-void `SyncContext` executes per-record callbacks on the producer thread. See [API contracts](docs/guides/API.md) for configuration and data lifetimes.

How the designs address the questions above:

| User concern | Design | Intended benefit |
|---|---|---|
| Business slows down without backpressure | Small producer pool, large consumer pool | Reduce write-side interference with business caches |
| Frequent shared-progress updates | Batch publication and handoff | Lower average producer submission overhead |
| Temporary consumer slowdowns | Independent copy stage into output storage | Return source blocks sooner |
| Typical load differs from peaks | Allocate on demand and reclaim excess free blocks when configured | Adjust memory use and burst capacity |
| Occasional large messages | Splitting and reassembly (planned) | Transfer large messages through a small producer pool |
| Selecting returned blocks | Reuse the most recently returned block first | Increase opportunities to reuse recently accessed memory |

## 4. Use cases

### 4.1 Where the design helps

#### 4.1.1 Sending batches of messages

**Scenario:** A calculation has finished and the application needs to send a sequence of results, logs, samples or events. The messages are ready; the producer wants to submit them quickly and resume other work.

**Benefit:** Consecutive submissions can have very low producer overhead. Compared with repeating a full submission path for every message, TwoBufQueue's batch-write option reduces repeated work so the business thread finishes the batch sooner.

[Performance comparison](docs/reports/comparison/performance_256m_zero_session_plateau.md) · [Usage example](examples/two_buf_queue/07_quick_write_muti_msg.cpp)

#### 4.1.2 Batch notification and processing are acceptable

**Scenario:** Log collection, telemetry and event aggregation can process a small batch together. Each message does not need to be received immediately after it is written.

**Benefit:** Compared with sharing progress and handing off every record, submitting a batch reduces coordination between the producer and background workers. Applications that accept batch arrival can leave more CPU time for producer business work while maintaining high throughput.

[Batch publication results](docs/reports/comparison/performance_256m_zero_publish_max_plateau.md) · [Per-record publication results](docs/reports/comparison/performance_256m_zero_publish1_plateau.md)

#### 4.1.3 Transferring larger messages or data blocks

**Scenario:** The producer needs to deliver a batch of sensor samples, assembled records or another contiguous data block for background processing, without doing subsequent computation or storage itself.

**Benefit:** Submit the range as a message to reduce application-level per-record dispatch and leave expensive processing to background workers. Consumer capacity for pending work is configured independently. Planned splitting and reassembly will let messages larger than one source block travel through a smaller producer pool.

[Usage and length limits](docs/guides/API.md) · [Configuration example](examples/two_buf_queue/08_all_parameters.cpp). Each message currently must fit one source block; a dedicated large-message benchmark is planned.

#### 4.1.4 Consumer speed varies

**Scenario:** The consumer writes files, runs batch calculations or calls an external service. Its speed varies, but the producer wants to keep running at its own pace.

**Benefit:** Keep more pending work at the consumer while keeping the producer pool small. Compared with enlarging one shared queue, TwoBufQueue makes it easier to balance temporary backlog capacity with producer memory use, reducing the effect of slower consumption on the business thread.

Capacity is finite; sustained overload or insufficient background copy progress still causes backpressure.

#### 4.1.5 Pursuing extreme producer hot-path performance

**Scenario:** A robotics controller, trading system or another demanding business thread sends a continuous batch of status updates, market data or events, then must immediately resume its core calculation.

**Benefit:** Compared with writing through a large single-layer message area, a small producer pool can reduce interference with business data in CPU caches. The benefit includes keeping subsequent business execution fast, as well as submitting messages quickly.

[Business-performance comparison](docs/reports/burst/performance_burst_from_empty.md): after a 16 MiB batch on this host, TwoBufQueue's business cost rises from about 13 ns/call to 18.2 ns/call. The report below states the conditions.

#### 4.1.6 Several producer–consumer combinations are needed

**Scenario:** Multiple business modules produce messages for one or more background workers. An application may also start with one producer and add concurrency later.

**Benefit:** One component covers single and multiple producers and consumers, reducing the need to assemble different components for each concurrency requirement. Each producer has its own writing endpoint and messages can be distinguished by source, easing application integration.

[Multiple-producer example](examples/two_buf_queue/03_multi_thread_writer.cpp) · [Consumer-worker configuration](examples/two_buf_queue/08_all_parameters.cpp)

### 4.2 Where the tradeoffs matter

#### 4.2.1 Every message must arrive immediately

**Scenario:** The consumer must receive each message as soon as possible. Individual send-to-receive time matters more than the producer's speed when submitting a batch.

**Drawback:** The two-layer path adds a copy, and batch handoff can introduce waiting. A more direct single-layer SPSC queue is usually a better fit for this goal. Faster producer submission does not mean the fastest individual-message arrival.

#### 4.2.2 Traffic is light and pending work is rare

**Scenario:** A few messages arrive occasionally, the consumer easily keeps up, and business-thread performance is not particularly demanding.

**Drawback:** The benefits of two layers may be small while copying, worker threads and configuration still have costs. A single-layer queue or direct call is usually simpler to integrate and maintain.

## 5. Getting started

### 5.1 Minimal example

Create a Writer and a queue, submit messages through the Writer, and process received data in a callback. The default path is asynchronous: the business callback runs on a consumer worker.

```cpp
#include <TwoBufQueue.hpp>
#include <cstdio>
#include <string_view>

int main() {
    //---------------1. Initialization--------------------------------------
    auto writer = TwoBufQueue::make_writer<TwoBufQueue::Writer<>>();
    // Callback that receives each message.
    auto on_record = [](std::int32_t, const std::byte* data,
                        std::size_t size) noexcept {
        std::printf("%.*s\n", static_cast<int>(size),
                    reinterpret_cast<const char*>(data));
    };
    TwoBufQueue::TwoBufQueue queue(on_record, writer);

    //---------------2. Producer writes-------------------------------------
    bool ok = true;
    for (std::string_view text : {"hello", "telemetry", "goodbye"})
        ok &= writer.write_mem(text.data(), static_cast<std::uint16_t>(text.size()));
    writer.flush();   // Publish and notify; does not wait for delivery.

    //---------------3. Finish----------------------------------------------
    writer.finish();  // Stop production and hand off the tail block.
    queue.stop();     // Drain copying and callbacks before leaving this scope.

    //---------------4. Check the result------------------------------------
    return ok && queue.error_code() == TwoBufQueue::ReadError::none ? 0 : 1;
}
```

The [async logger](examples/two_buf_queue/09_async_logger.cpp) adds bounded buffers, pressure statistics, a slow-sink option and verification of the saved file. It distinguishes queue `flush()` from flushing the file.

Currently requires **C++20**. See [build and integration](#53-build-and-integrate) below and the [API guide](docs/guides/API.md) for parameters, message formats and lifetimes.

### 5.2 Choose a starting point

| Need | Example |
|---|---|
| Submit individual records | [01_basic](examples/01_basic.cpp) |
| Receive grouped data in one callback | [02_batch_callback](examples/two_buf_queue/02_batch_callback.cpp) |
| Multiple producer threads | [03_multi_thread_writer](examples/two_buf_queue/03_multi_thread_writer.cpp) |
| Read source blocks directly | [04_layer1_callback](examples/two_buf_queue/04_layer1_callback.cpp) |
| Attach sequence numbers | [05_seq_writer](examples/two_buf_queue/05_seq_writer.cpp) |
| Execute callbacks on the producer | [06_sync_writer](examples/two_buf_queue/06_sync_writer.cpp) |
| Write many records in a loop | [07_quick_write_muti_msg](examples/two_buf_queue/07_quick_write_muti_msg.cpp) |
| Tune construction parameters | [08_all_parameters](examples/two_buf_queue/08_all_parameters.cpp) |
| Write application logs asynchronously | [09_async_logger](examples/two_buf_queue/09_async_logger.cpp) |

The project also includes standalone examples for the memory pool and other building blocks. See the [complete example index](examples/README.md).

### 5.3 Build and integrate

Requires CMake 3.20+, a C++20 compiler and thread support. Default detailed tests require Python 3.9+. No third-party runtime dependency. The macOS implementation requires **macOS 14.4+ and a supporting SDK**.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

For a vendored checkout:

```cmake
add_subdirectory(external/TwoBufQueue)
target_link_libraries(your_app PRIVATE TwoBufQueue::TwoBufQueue)
```

Tests and examples default to OFF when embedded in another project. For an installed package:

```cmake
find_package(TwoBufQueue 0.1 CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE TwoBufQueue::TwoBufQueue)
```

[Integration instructions](docs/guides/INTEGRATION.md) cover installation, relocatable packages and `FetchContent`. The CMake package version is 0.1.0; no release tag is implied by that metadata.

### 5.4 Usage boundaries

- One producer thread per Writer. Register every Writer before starting the readers; each source queue has one reader owner. There is no global FIFO across producers.
- Check write results. Bounded pools can fill: the default Drop policy rejects a record immediately; Block waits up to its configured timeout and can also fail.
- Default records carry a two-byte length prefix, with at most 65,535 payload bytes. Each complete record must fit its source block. Object bytes are an in-process representation, not a serialization format.
- Do not retain callback pointers. Copy data into application-owned storage before dispatching work elsewhere. `noexcept` callbacks must record or handle their own errors.
- With current-block scanning disabled, `flush()` cannot deliver a partial current block. Use an explicit handoff or `finish()` for the tail. With scanning enabled, publishing and scheduling still affect delivery latency.
- Stop and join producers, call each Writer's `finish()`, then `queue.stop()` and check its error. Queue flush does not wait for delivery or imply durable storage.

See [API details](docs/guides/API.md), [test evidence](docs/development/TESTING.md), and [contributing](CONTRIBUTING.md). Linux/Windows/macOS CI is configured; the saved local results identify which platforms actually ran.

## 6. Roadmap

- **Large-message splitting and reassembly**: split messages larger than one source block across blocks and reconstruct them at the consumer, allowing a small source block size with larger messages. The goal is to reduce large-message writes interfering with producer business caches. Not implemented yet; dedicated measurements will accompany the feature.
- **Layer-2 buffer serialization to files and readback**: as layer-2 memory approaches its limit, serialize some pending messages to files, then read them back for consumption. File storage and readback run in the background at layer 2. The goal is to extend backlog capacity while keeping producer memory small. Not implemented yet; the file format, memory/file transition policy, readback and integrity checks will accompany the feature.
- **SeqWriter backend ordering and two-stage processing**: stage one processes messages in parallel across multiple threads and retains their original sequence numbers. Stage two gathers results that may finish out of order, orders them by message sequence number, and serially executes the work that requires message order. The goal is to combine parallel processing with ordered final execution. SeqWriter currently carries sequence numbers; this backend execution pipeline is not implemented yet. Result buffering and missing-sequence handling will accompany the feature.
- **More C++ standard versions**: the current minimum remains C++20; compatibility implementations and build coverage are planned.

Licensed under [Apache-2.0](LICENSE), copyright © 2026 chuan lu. See [NOTICE](NOTICE).
