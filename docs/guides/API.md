# API usage guide

Create a producer's `writer`, then give the writer and a message-receiving callback to `TwoBufQueue`. Send through the writer. By default, the queue copies data and calls the callback in the background.

[Project overview](../../README.md) · [中文接口指南](API.zh-CN.md) · [Integration](INTEGRATION.md) · [Runnable examples](../../examples/README.md)

## 1. Choose the interface

| What you need | Use | Example |
|---|---|---|
| Send and receive individual records | `Writer<>` + `TwoBufQueue` | [Basic](../../examples/01_basic.cpp) |
| Submit many records in a loop | `Writer<UINT32_MAX>` + `WriteSession` | [Loop](../../examples/two_buf_queue/07_quick_write_muti_msg.cpp) |
| Add a sequence number | `SeqWriter` | [Sequence](../../examples/two_buf_queue/05_seq_writer.cpp) |
| Receive grouped data in one callback | `TwoBufQueueBatch` | [Grouped-data callback](../../examples/two_buf_queue/02_batch_callback.cpp) |
| Execute the callback on the producer | Non-void `SyncContext` | [Synchronous](../../examples/two_buf_queue/06_sync_writer.cpp) |
| Read source blocks without layer-2 copying | Compose `ReadWorker` and a source callback | [Direct read](../../examples/two_buf_queue/04_layer1_callback.cpp) |

Start with the complete example below; read configuration when you need to tune it.

## 2. A complete send-and-receive example

```cpp
#include <TwoBufQueue.hpp>
#include <cstdio>

int main() {
    // Create the sending endpoint and its source pool.
    auto writer = TwoBufQueue::make_writer<TwoBufQueue::Writer<>>();

    // Callback that receives each message; data and size describe its payload.
    auto on_record = [](std::int32_t source_id, const std::byte* data,
                        std::size_t size) noexcept {
        std::printf("source=%d: %.*s\n", static_cast<int>(source_id),
                    static_cast<int>(size), reinterpret_cast<const char*>(data));
    };

    // Construction starts background processing. The writer outlives the queue.
    TwoBufQueue::TwoBufQueue queue(on_record, writer);
    const bool ok = writer.write_mem("hello", 5);
    writer.finish(); // End production and hand off the tail.
    queue.stop();    // Finish background processing.
    return ok && queue.error_code() == TwoBufQueue::ReadError::none ? 0 : 1;
}
```

Successful submission does not mean the callback has already run. Initialization can throw for invalid configuration or allocation failure; handle this at the application's initialization boundary.

## 3. Sending messages

| Function | Suitable input | Result |
|---|---|---|
| `writer.write(value...)` | Trivially copyable values, such as integers or simple structs | `true` submitted; `false` not written |
| `writer.write_mem(data, len)` | Strings, byte arrays or dynamically stored bytes | `true` submitted; `false` not written |

`write` tightly concatenates its arguments into one message. It does not accept pointers or native arrays as payload values. Use `write_mem` for the pointed-to contents, and for `std::string::data()` plus length rather than the `std::string` object itself.

```cpp
const std::uint64_t value = 42;
const bool value_ok = writer.write(value);
const bool text_ok = writer.write_mem("hello", 5);
// Check each result; a failed message did not enter the queue.
```

Asynchronous submission copies the input; reuse that input after the call returns. Source memory must be valid and must not overlap the destination.

The default `Writer<>` stores a two-byte length prefix and accepts at most 65,535 payload bytes. The entire record must also fit one source block. Use a wider length type, such as `Writer<1, true, std::uint32_t>`, when needed. Ensure the length fits that type before calling `write_mem`; a narrowing cast can silently truncate it. Oversized records are not automatically split.

Ordinary framed Writers allow empty messages; unframed Writers reject empty payloads. **Session `write_mem` requires a positive length.** Object bytes are an in-process representation, not a cross-machine serialization format.

## 4. The receiving callback

```cpp
auto on_record = [](std::int32_t source_id, const std::byte* data,
                    std::size_t size) noexcept {
    // Process one message here.
};
```

| Argument | Meaning |
|---|---|
| `source_id` | The configured Writer identifier, not an OS thread ID |
| `data` | This message's payload, without its length prefix |
| `size` | Payload byte count |

The default is one background consumer worker. With multiple workers, callbacks can run concurrently; protect shared application state.

**The pointer is borrowed only for this callback.** Copy into application-owned storage before submitting work to an application thread pool. Do not leave borrowed `data` or `DataBatch` pointers with asynchronous tasks. The queue does not directly accept an external executor.

Validate the length and use `std::memcpy` to decode a struct; callback addresses may be unaligned. Per-record callbacks return `void` and have no refusal/retry result. Handle errors inside the `noexcept` callback.

## 5. Publication, handoff and shutdown

| Operation | What it does | Can production continue? |
|---|---|---|
| `writer.flush()` | Publishes written bytes and requests reading; keeps the current block active | Yes |
| `writer.finish_block()` | Hands off the current block; the next write acquires another | Yes |
| `writer.finish()` | Ends this Writer and hands off its tail | No |
| `queue.stop()` | Finishes copying and consumption after production ends | Do not write again |

Flush neither waits for callbacks nor makes file data durable. With current-block scanning enabled, published portions can be read. With scanning disabled, a partial current block needs `finish_block()` or final `finish()` even if you flushed it.

While a Session is active, use `session.flush()` rather than direct Writer operations. Release the Session before handing off or ending production.

## 6. Configure the two capacities separately

Each Writer owns a source pool. The queue owns a separate output pool.

```cpp
auto writer = TwoBufQueue::make_writer<TwoBufQueue::Writer<>>({
    .block_size = 4096,
    .pool_min = 4,
    .pool_max = 4, // Source data budget: about 16 KiB.
});
TwoBufQueue::TwoBufQueueOptions options;
options.output_block_size = 64 * 1024;
options.output_pool_min = 2;
options.output_pool_max = 16; // Output data budget: about 1 MiB.
// TwoBufQueue::TwoBufQueue queue(on_record, writer, options);
```

These budgets exclude metadata, queue slots and application inputs. An output block must hold the largest source range; normally make it at least as large as a source block.

### 6.1 On-demand allocation and free-block reclamation

`pool_min` sets the initial count and `pool_max` the total limit. When no reusable block is available, the pool can allocate a new one as needed; it does not have to allocate its maximum capacity up front.

The underlying `WarmPool` also accepts `max_free_count`, a free-block retention limit. On return, when the free count reaches that limit and the total count exceeds `min_count`, excess blocks enter reclamation. Borrowed blocks are not reclaimed.

The `make_writer` and `TwoBufQueueOptions` convenience configurations retain free blocks by default and do not expose this retention limit. For source-pool reclamation, construct `WriterPool(min_count, max_count, factory, max_free_count)` manually, then connect a source queue and Writer. See [all parameters](../../examples/two_buf_queue/08_all_parameters.cpp).

### 6.2 Source: WriterSourceConfig

| Field | Default | Meaning |
|---|---|---|
| `block_size` | 64 KiB | Bytes per source block, including record prefixes |
| `pool_min` / `pool_max` | 2 / 8 | Initial and maximum block counts for this Writer |
| `thread_id` | 0 | Unique source identifier within the queue |
| `queue_capacity` | 256 | Block handoff record capacity, at least 2; not message count |
| `backpressure.mode` | `Drop` | Fail immediately or wait when no block is available |
| `backpressure.spin_count` | 1,000 | Short retries before waiting in Block mode |
| `backpressure.wait_get_timeout` | 5 seconds | Maximum Block wait, including short retries |

Drop rejects this message; the application chooses retry or discard. Block can fail after its timeout. `writer.backpressure_stats()` reports pressure events, rejected records and blocking events. Add the source budgets of all Writers when calculating total memory.

### 6.3 Output and workers: TwoBufQueueOptions

| Field | Default | Meaning |
|---|---|---|
| `output_block_size` | 64 KiB | Bytes per output block |
| `output_frame_capacity` | 64 | Source ranges per output block; a range can contain many records |
| `output_pool_min` / `output_pool_max` | 2 / 8 | Initial and maximum output block counts |
| `output_queue_capacity` | 8 | Output handoff capacity, separate from pool capacity |
| `read_layer1_thread_count` | 1 | Copy worker count, capped at the number of Writers |
| `output_mode` | `MPSC` | Copy/consumer concurrency mode |
| `output_reader_count` | 1 | Consumer worker count; multiple workers need SPMC/MPMC |
| `output_reader_mode` | `WaitSignal` | Wait for notification; Polling repeatedly checks for work |
| `output_retry_wait` | 1 ms | Retry delay after a batch callback refuses data in wait mode |
| `cancel_claim_mode` | `QueueTail` | Reclaim ordering after batch refusal |
| `output_enable_stats` | `false` | Enable output handoff statistics |

Maximum block counts must be positive; initial counts cannot exceed them. Worker counts must match the selected mode. See [all parameters](../../examples/two_buf_queue/08_all_parameters.cpp) for complete constraints.

## 7. Reading the active block

The current block is the source block still being written. `options.read_config.is_scan_current` controls whether the background worker reads its published portion.

| Setting | Behavior | Suitable goal |
|---|---|---|
| `true`, default | Read published current-block bytes and completed blocks | Receive sparse messages even after the producer becomes idle |
| `false` | Read only handed-off blocks | Accept batch arrival and prioritize submission performance |

The first Writer template argument sets publication frequency: `Writer<1>` publishes every record; `Writer<10>` every ten; `Writer<UINT32_MAX>` only on flush, block switch or finish.

**Publishing a position is different from waking a worker.** Flush, block switches and finish request reading. A published position does not mean the consumer is scheduled immediately. Flush an unfinished publication batch before becoming idle; with scanning off, hand off the current block instead.

Common `read_config` fields:

| Field | Default | Meaning |
|---|---|---|
| `idle_rescan_interval` | 5 ms | Initial current-block scan interval |
| `idle_rescan_max_interval` | 30 ms | Maximum interval after consecutive empty scans |
| `idle_spin_count` | 1,000 | Short polling before waiting |
| `work_wait_min_interval` | 1 ms | Minimum ordinary wait; notifications can wake it early |
| `worker_cpu` | -1 | OS scheduling by default; affinity availability is platform-specific |
| `finish_timeout` | 5 seconds | Total copy shutdown timeout; check errors on expiry |
| `finish_retry_interval` | 1 ms | Retry interval during unfinished shutdown |

Data restores the initial scan interval; empty scans gradually increase it. Shorter intervals do not guarantee equally precise OS scheduling. A finish timeout cannot forcibly interrupt an application callback.

## 8. Consecutive writes: WriteSession

A Session writes one block. When space runs out, it returns a result and the application's outer loop chooses a new block. This interface is available for asynchronous `Writer<UINT32_MAX>` and corresponding SeqWriters, not other publication counts or synchronous Writers.

| Interface | Meaning |
|---|---|
| `get_current_session(timeout)` | Reuse the current block, or try to acquire one |
| `get_next_session(timeout)` | Hand off the current block, then acquire the next |
| `session.write(...)` / `write_mem(...)` | Write one record without an implicit block switch |
| `session.flush()` | Publish and notify; keep the Session and block active |
| `session = {}` or scope exit | Return the cursor to the Writer; no automatic flush |

Timeout defaults to zero, one attempt. A positive timeout allows acquisition retries. Check `if (!session)` before writing. In the inner loop, advance the message index only on success. On insufficient space, leave that loop, release the old Session and acquire the next block outside it. See the [complete nested-loop example](../../examples/two_buf_queue/07_quick_write_muti_msg.cpp).

| SessionWriteResult | Meaning | Action |
|---|---|---|
| `success` | Record written | Advance message index |
| `insufficient_space` | Not enough room in this block | Leave inner loop; switch outside it |
| `invalid_argument` | Zero length in `write_mem` | Fix the input; source address validity remains the caller's responsibility |
| `exceeds_block_capacity` | Complete record exceeds a whole block | Change message or configuration; a fresh block will not fix it |
| `error` | Invalid Session or another error | Stop this submission and handle it |

One active Session per Writer. It must not outlive the Writer. Do not directly operate on the Writer while a Session is active. Release the Session before ending production; flush is the caller's choice.

## 9. Multiple producers and consumers

One Writer per producer thread, with unique source IDs and the same Writer type. Supply every Writer at construction; the running queue does not accept additional Writers.

```cpp
auto writer0 = TwoBufQueue::make_writer<TwoBufQueue::Writer<>>({.thread_id = 0});
auto writer1 = TwoBufQueue::make_writer<TwoBufQueue::Writer<>>({.thread_id = 1});
TwoBufQueue::TwoBufQueue queue(on_record, {&writer0, &writer1});
// Thread 0 operates writer0 only; thread 1 operates writer1 only.
```

There is no global order across producers, and parallel consumers need not complete callbacks in order. Multiple consumers share the work; they do not receive a broadcast copy of every message.

| Output mode | Copy workers, the output producers | Consumer workers |
|---|---|---|
| `SPSC` | 1 | 1 |
| `MPSC` | One or more | 1 |
| `SPMC` | 1 | One or more |
| `MPMC` | One or more | One or more |

Use `read_layer1_thread_count` for copying and `output_reader_count` for consumption. Protect shared callback state. [Multiple producers](../../examples/two_buf_queue/03_multi_thread_writer.cpp) · [Worker configuration](../../examples/two_buf_queue/08_all_parameters.cpp).

## 10. Synchronous, sequence and batch callbacks

### 10.1 Execute on the producer

Define the callback first and pass its concrete type through SyncContext:

```cpp
auto on_record = [](std::int32_t, const std::byte*, std::size_t) noexcept {
    // This message callback runs on the sending thread.
};
using Context = TwoBufQueue::SyncContext<decltype(on_record)>;
auto writer = TwoBufQueue::make_writer<
    TwoBufQueue::Writer<1, true, std::uint16_t, Context>>();
TwoBufQueue::TwoBufQueue queue(on_record, writer);
const bool ok = writer.write(std::uint64_t{42}); // Callback completes before return.
writer.finish();
queue.stop();
```

The default `TSyncContext=void` selects asynchronous delivery; a concrete Context selects synchronous delivery. No output pool or reader workers are created for this synchronous path. One Writer needs no mutex; multiple Writers serialize callbacks through a shared mutex. A multi-Writer callback must not reenter the same queue. A slow callback directly delays the producer.

An ordinary Writer with `output_mode=Sync` is rejected; it does not select this path. `TwoBufQueueBatch` has a separate Sync mode that executes a batch callback on the copy worker.

### 10.2 Add a sequence number

SeqWriter's callback adds a sequence: `void(source_id, sequence, data, size) noexcept`. Sequence allocation does not promise global callback order. Its final Context parameter selects synchronous delivery in the same way. See the [sequence example](../../examples/two_buf_queue/05_seq_writer.cpp).

### 10.3 Process a batch directly

TwoBufQueueBatch passes a borrowed DataBatch. One DataFrame can hold several records. Return true to accept the batch or false to retain it for retry; never keep borrowed pointers outside the callback.

`Writer<..., false>` omits length prefixes and requires application-defined boundaries and a batch interface. A direct layer-1 callback also borrows a range: on acceptance the reader advances and returns completed blocks. Do not consume or return them yourself. [Batch example](../../examples/two_buf_queue/02_batch_callback.cpp) · [Direct read](../../examples/two_buf_queue/04_layer1_callback.cpp).

## 11. End production safely

1. Stop production and join producer threads.
2. Release active Sessions.
3. Call every Writer's finish.
4. Call queue.stop and check queue.error_code.
5. Wait for any tasks that the callback sent to an application pool using owned data.

Keep Writers, callbacks and their resources alive for the work they serve. A timeout or error is not proof of complete delivery. File flush, closing and durable-storage confirmation are application responsibilities. The [asynchronous logger](ASYNC_LOGGING.md) demonstrates shutdown.

## 12. Composing lower-level components

Normal use lets make_writer own source resources. For custom factories or statistics, explicitly create WriterPool, make_writer_queue(pool, capacity, enable_stats) and Writer(queue, id, backpressure). All dependencies must outlive the running pipeline.

Each source queue has one read owner. Source ranges, output frames and records are different units. Let the matching reader manage confirmation, retries and return; do not return the same resource twice. See [component examples](../../examples/README.md) and [all parameters](../../examples/two_buf_queue/08_all_parameters.cpp).
