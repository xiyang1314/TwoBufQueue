# TwoBufQueue

**TwoBufQueue is a C++ producer–consumer message queue component for transferring data between threads at high speed.**

Its two-layer buffer design prioritizes producer hot-path performance. Batch publication and transfer let producers use small buffers while consumers use larger buffers, reducing the effect of message transfer on business execution. Supports single or multiple producers and consumers. Currently requires C++20.

[简体中文](https://xiyang1314.github.io/TwoBufQueue/README.zh-CN.html) · [Full guide](https://xiyang1314.github.io/TwoBufQueue/README_DETAIL.html) · [API guide](https://xiyang1314.github.io/TwoBufQueue/docs/guides/API.html) · [Examples](https://xiyang1314.github.io/TwoBufQueue/examples/README.html) · [Benchmarks](https://xiyang1314.github.io/TwoBufQueue/docs/reports/PERFORMANCE.html)

**Contents**

- [1. A few questions](#1-a-few-questions)
  - [1.1 Why can business work slow down after sending, even without backpressure?](#11-why-can-business-work-slow-down-after-sending-even-without-backpressure)
  - [1.2 How much faster can batch submission be for tiny messages?](#12-how-much-faster-can-batch-submission-be-for-tiny-messages)
  - [1.3 When message buffers are full, must the producer wait?](#13-when-message-buffers-are-full-must-the-producer-wait)
  - [1.4 How can large messages be sent more efficiently?](#14-how-can-large-messages-be-sent-more-efficiently)
  - [1.5 Small buffers cause backpressure; large buffers waste memory. What should you do?](#15-small-buffers-cause-backpressure-large-buffers-waste-memory-what-should-you-do)
- [2. How fast is it?](#2-how-fast-is-it)
  - [2.1 Producer submission throughput](#21-producer-submission-throughput)
  - [2.2 Business performance after a continuous message batch](#22-business-performance-after-a-continuous-message-batch)
- [3. When does it fit?](#3-when-does-it-fit)
  - [3.1 Useful scenarios](#31-useful-scenarios)
  - [3.2 Tradeoffs to consider](#32-tradeoffs-to-consider)
- [4. Get started](#4-get-started)
  - [4.1 Minimal example](#41-minimal-example)
  - [4.2 Build and integrate](#42-build-and-integrate)
- [5. Full documentation](#5-full-documentation)

---

## 1. A few questions

### 1.1 Why can business work slow down after sending, even without backpressure?

The queue has room and sends never block, so everything appears normal. Yet after sending a continuous batch, the same business code can take several times longer to execute. If the producer never waited, where did the extra time go?

### 1.2 How much faster can batch submission be for tiny messages?

Copying an 8 or 16 B message should be quick. The smaller the message, the more the other work involved in sending deserves attention. With the same amount of data and the same number of messages, how much can performance differ between individual and batch submission?

### 1.3 When message buffers are full, must the producer wait?

Consumption slows down, messages accumulate and the buffers eventually fill. Waiting interrupts business execution; dropping messages can lose important records. Could there be another place for pending messages while keeping memory use bounded?

### 1.4 How can large messages be sent more efficiently?

Most records may be only a few dozen bytes, with an occasional snapshot, sample batch or data block measuring several MiB. A single send then writes a broad memory range. How can it transfer quickly while minimizing its effect on subsequent business work?

### 1.5 Small buffers cause backpressure; large buffers waste memory. What should you do?

Small buffers may not cover bursts and can make the producer wait. Large buffers may retain memory for peaks that happen only occasionally. When normal traffic and peak traffic differ greatly, how can burst capacity and memory use be balanced?

> These questions guide the design and continued improvement of TwoBufQueue. See [Problems and solutions](https://xiyang1314.github.io/TwoBufQueue/README_DETAIL.html#problems-and-solutions) for the approaches and planned features. Continue below for the performance measurements.

## 2. How fast is it?

### 2.1 Producer submission throughput

The four charts compare producer submission throughput in **million messages/s; higher is better**. TwoBufQueue uses `WriteSession`; consumers run concurrently.

![Producer submission throughput for 8, 16, 32 and 64 B messages](docs/assets/session_write_throughput.svg)

[Full Session results](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_256m_zero_session_plateau.html) · [Ordinary write, batch publication](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_256m_zero_publish_max_plateau.html) · [Ordinary write, per-record publication](https://xiyang1314.github.io/TwoBufQueue/docs/reports/comparison/performance_256m_zero_publish1_plateau.html)

### 2.2 Business performance after a continuous message batch

Every queue follows the same sequence: **normal business work → send a continuous message batch → immediately repeat the same business work**. The chart shows post-send cost / normal cost; lower is better.

![Post-send business cost relative to normal after 2, 4, 8 and 16 MiB batches](docs/assets/burst_business_ratio.svg)

Normal business work costs about **13 ns/call** on this host. After a 16 MiB batch, TwoBufQueue costs **18.2 ns/call**, ReaderWriterQueue 58.2, rigtorp 42.5 and atomic_queue 99.8. [Full experiment and parameters](https://xiyang1314.github.io/TwoBufQueue/docs/reports/burst/performance_burst_from_empty.html)

These are separate experiments on Apple M3. Submission uses 256 MiB per TwoBufQueue layer and callback loop count 0. Business impact uses a 16 KiB producer pool, a 256 MiB consumer pool and callback loop count 64. Native queues use 256 MiB of message storage. Three-run means, high QoS and no hard CPU affinity; see the reports for timing scope and limitations.

## 3. When does it fit?

### 3.1 Useful scenarios

If the questions in chapter 1 interest you, or you have encountered similar problems, consider TwoBufQueue.

In practical terms: the producer runs on the hot-path thread and the consumer runs on a supporting thread. You want the hot path to remain stable and efficient while letting the producer send messages at high speed, as in low-latency applications such as robotics, sensors and quantitative trading.

**I will continue improving this component. You are welcome to share related requirements and ideas at any time. I will do my best to implement requests that are broadly useful to the community.**

### 3.2 Tradeoffs to consider

Two-layer transfer adds copying and background work. A single-layer queue may fit better when individual send-to-receive time is the main objective, or when traffic and processing are light. Capacity is finite; insufficient copy progress or sustained overload still causes backpressure.

[Detailed benefits, tradeoffs and design comparison](https://xiyang1314.github.io/TwoBufQueue/README_DETAIL.html)

## 4. Get started

### 4.1 Minimal example

Submit through a Writer and receive messages in the `on_record` callback.

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

### 4.2 Build and integrate

Requires **CMake 3.20+, a C++20 compiler and thread support**. No third-party runtime dependency. macOS requires 14.4+ and a supporting SDK; detailed tests additionally require Python 3.9+.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Link the CMake target `TwoBufQueue::TwoBufQueue`. [Installation and integration](https://xiyang1314.github.io/TwoBufQueue/docs/guides/INTEGRATION.html) · [More examples](https://xiyang1314.github.io/TwoBufQueue/examples/README.html)

## 5. Full documentation

| What you need | Start here |
|---|---|
| Two-layer design, use cases and roadmap | [Full guide README_DETAIL](https://xiyang1314.github.io/TwoBufQueue/README_DETAIL.html) |
| Writing, publication, callbacks, threads and parameters | [API guide](https://xiyang1314.github.io/TwoBufQueue/docs/guides/API.html) |
| Session, multiple producers and synchronous callbacks | [Examples](https://xiyang1314.github.io/TwoBufQueue/examples/README.html) |
| Measurement scope, parameters and reproduction | [Benchmark methodology](https://xiyang1314.github.io/TwoBufQueue/docs/reports/PERFORMANCE.html) |
| Application logging through the queue | [Async logging guide](https://xiyang1314.github.io/TwoBufQueue/docs/guides/ASYNC_LOGGING.html) |

Apache-2.0. [License](LICENSE) · [Contributing](https://xiyang1314.github.io/TwoBufQueue/CONTRIBUTING.html)
