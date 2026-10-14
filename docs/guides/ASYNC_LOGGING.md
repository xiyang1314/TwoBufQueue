# Asynchronous logging

[English](ASYNC_LOGGING.md) · [简体中文](ASYNC_LOGGING.zh-CN.md)

[Example source](../../examples/two_buf_queue/09_async_logger.cpp) · [Integration](INTEGRATION.md)

The application submits a trivially copyable record containing sequence, request ID and text. The copy worker moves source ranges into output blocks; an independent callback thread formats records and writes the file. The callback checks the record length and copies its bytes with `memcpy`, because callback addresses may be unaligned.

The example deliberately uses only two 4 KiB blocks in each layer to make full-buffer behavior easy to observe. It demonstrates usage and is not a pure hot-path benchmark.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target brq_tbq_09_async_logger
./build/brq_tbq_09_async_logger ./application.log
./build/brq_tbq_09_async_logger ./slow.log --slow-sink
./build/brq_tbq_09_async_logger ./drop.log --slow-sink --drop
```

With Visual Studio, run `build/Release/brq_tbq_09_async_logger.exe`. The example truncates the selected output file. The default name is `twobufqueue-example.log` in the working directory.



The default Block policy waits for space for up to two seconds. A failure is counted and reported; default mode fails the run if any record was rejected. Drop mode counts rejected records and continues. The optional slow sink sleeps for 20 ms in the first callback; pressure counts depend on scheduling. The saved file is reopened and checked for every accepted sequence, text, order and exact record count. Rejected records are never counted as delivered.

## 1. Flush and shutdown

1. `writer.flush()` publishes pending bytes and notifies the copy worker. Here current-block scanning is enabled. It neither waits for file callbacks nor calls `fflush`.
2. After production ends, `writer.finish()` hands off the last source block. If producers are separate threads, stop and join them before final shutdown.
3. `queue.stop()` drains copying and output callbacks; check `queue.error_code()`.
4. Only then call file `fflush`, close the file and inspect I/O errors. These file operations do not provide an `fsync` durability guarantee.

`flush` does not confirm durable file storage. With current-block scanning disabled, a flush notification does not read an unfinished block: explicitly hand it off or finish the writer. The per-record callback returns void, so it cannot request a retry by returning false. This example records I/O failures in state and checks them after draining instead of throwing from a noexcept callback.

This small example has no file rotation, timestamp formatting, crash recovery or automatic I/O retry. Those policies belong in a real logging application, outside the queue's data-transfer contract.
