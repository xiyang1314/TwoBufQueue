# API contracts

[README](../README.md) · [中文详细接口](API.zh-CN.md) · [Construction example](../examples/two_buf_queue/08_all_parameters.cpp)

## Records and ownership

`make_writer<Writer<>>()` owns its source pool and handoff queue. Configure source block size, pool limits, queue capacity, source ID and backpressure with `WriterSourceConfig`. Configure output blocks, output pool, copy workers and callback readers separately with `TwoBufQueueOptions`. All Writers must exist throughout the pipeline's lifetime and be registered before readers start. Each Writer has one producer thread and each source queue one read owner.

`write(args...)` tightly copies the bytes of trivially copyable values; `write_mem(data, length)` copies an explicit byte range. The default record has a two-byte little-endian payload-length prefix and at most 65,535 payload bytes. A complete record, including prefixes, must fit its source block. Use `memcpy` to decode potentially unaligned callback data. This layout is an in-process object representation, not a network or persistence format.

The per-record callback signature is `void(std::int32_t source_id, const std::byte* data, std::size_t size) noexcept`. Default `TwoBufQueue` copies ranges on a background worker and invokes callbacks on an independent thread. Callback data is borrowed for the duration of the call. Copy into application-owned memory before dispatching asynchronous work. A void record callback has no refusal/retry return value; handle errors inside it. Default callback delivery is serial; multiple callback readers require the appropriate SPMC/MPMC output mode and a thread-safe callback.

`TwoBufQueueBatch` passes a borrowed `DataBatch`, whose frames are source ranges containing potentially multiple records. Its boolean callback can accept or retain a batch for retry; it must not keep the borrowed pointers after returning. Direct source callbacks have their own range-confirmation contract; use [04_layer1_callback](../examples/two_buf_queue/04_layer1_callback.cpp) when composing that path.

## Publication and bounded buffers

The first `Writer` template argument controls publication frequency. Its maximum `std::uint32_t` value advances only the local cursor on ordinary writes and publishes on flush or block handoff. `TwoBufQueueOptions::read_config.is_scan_current` controls reading published portions of the active block. With scanning disabled, flush still publishes/notifies, but a partial current block is delivered only after explicit handoff or finish. Tune publication, read intervals and memory limits together.

Check every write result. Drop is the default pressure policy and rejects a record when a new block cannot be acquired. Block waits, including configured spinning, for up to `wait_get_timeout`, then can still return false. `backpressure_stats()` reports pressure events, rejected records and blocking events; these counters are updated on pressure paths. Queue capacity and pool capacity are separate limits.

## WriteSession

Use [07_quick_write_muti_msg](../examples/two_buf_queue/07_quick_write_muti_msg.cpp) for many records in a tight loop. `get_current_session(timeout)` obtains the current block; an empty result means acquisition failed. A Session writes within that single block and never implicitly switches it.

`SessionWriteResult` distinguishes success, insufficient space, invalid dynamic-memory arguments, a record exceeding the entire block capacity, and other errors. On insufficient space, leave the inner loop; release the old Session and explicitly acquire `get_next_session(timeout)` in the outer loop. Advance the message index only on success. A too-large record will not fit a fresh block either, so handle that error instead of looping indefinitely.

Releasing or replacing a Session synchronizes its cursor with the Writer; it does not flush or hand off a block. `session.flush()` is explicit and leaves the Session usable. Release active Sessions before `writer.finish()`. Session acquisition/switching and ordinary Writer calls are operations of the same producer thread, with one active Session at a time.

## Sequence and synchronous callbacks

[SeqWriter](../examples/two_buf_queue/05_seq_writer.cpp) adds a sequence argument: `void(std::int32_t source_id, TSeq sequence, const std::byte*, std::size_t) noexcept`. Sequence assignment does not promise global callback order across Writers.

For a callback on the producer thread, pass `SyncContext<decltype(callback)>` as the final Writer / SeqWriter template parameter; its default `void` selects asynchronous delivery. [06_sync_writer](../examples/two_buf_queue/06_sync_writer.cpp) shows the complete setup. A single Writer invokes without taking a mutex; multiple Writers share a context that serializes callbacks with a mutex. The context preserves the callback's concrete type. Register the complete Writer set at construction, and do not reenter the same synchronous queue from a multi-Writer callback.

`TwoBufQueueOptions::output_mode=Sync` does not select per-record producer-thread delivery for an ordinary Writer; that combination is rejected. `TwoBufQueueBatch` has a separate batch callback execution policy. Synchronous delivery cannot isolate the producer from slow callbacks.

## Shutdown

Stop and join producer threads, release their Sessions, call each Writer's `finish()`, then `queue.stop()` and check `error_code()`. Keep the callback and its external resources alive until stop returns. Flush is a publication request, not a wait for callbacks or a durable-file guarantee. The [async logger](ASYNC_LOGGING.md) demonstrates file I/O and its separate error checks.
