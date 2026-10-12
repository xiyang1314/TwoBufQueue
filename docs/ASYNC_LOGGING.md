# Asynchronous logging / 异步日志

[Example source](../examples/two_buf_queue/09_async_logger.cpp) · [Integration](INTEGRATION.md)

The application submits a trivially copyable record containing sequence, request ID and text. The copy worker moves source ranges into output blocks; an independent callback thread formats records and writes the file. The callback checks the record length and copies its bytes with `memcpy`, because callback addresses may be unaligned.

业务线程提交序号、请求编号和日志正文。第二层复制线程与写文件的回调线程分开。示例采用每层两个 4 KiB 块，故意限制容量，让缓存满的行为容易观察；它是用法示例，不能作为纯热路径性能测试。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target brq_tbq_09_async_logger
./build/brq_tbq_09_async_logger ./application.log
./build/brq_tbq_09_async_logger ./slow.log --slow-sink
./build/brq_tbq_09_async_logger ./drop.log --slow-sink --drop
```

With Visual Studio, run `build/Release/brq_tbq_09_async_logger.exe`. The example truncates the selected output file. The default name is `twobufqueue-example.log` in the working directory.

默认使用 Block，最多等待 2 秒；如果仍写入失败，示例报告拒绝条数并以非零退出。`--drop` 改用 Drop：缓存满时 `write` 返回 false，应用记录拒绝条数并继续。`--slow-sink` 让第一次文件回调暂停 20 ms，用于观察压力统计，正常运行不加这个暂停。确切压力次数由线程调度决定。

The default Block policy waits for space for up to two seconds. A failure is counted and reported; default mode fails the run if any record was rejected. Drop mode counts rejected records and continues. The optional slow sink sleeps for 20 ms in the first callback; pressure counts depend on scheduling. The saved file is reopened and checked for every accepted sequence, text, order and exact record count. Rejected records are never counted as delivered.

## Flush and shutdown / flush 与退出

1. `writer.flush()` publishes pending bytes and notifies the copy worker. Here current-block scanning is enabled. It neither waits for file callbacks nor calls `fflush`.
2. After production ends, `writer.finish()` hands off the last source block. If producers are separate threads, stop and join them before final shutdown.
3. `queue.stop()` drains copying and output callbacks; check `queue.error_code()`.
4. Only then call file `fflush`, close the file and inspect I/O errors. These file operations do not provide an `fsync` durability guarantee.

`flush` 不是文件落盘确认。关闭 current 扫描时，flush 通知也不会读取未完成块，必须显式交接或 finish。逐条回调返回 void，不能以返回 false 要求重试；示例通过状态记录 I/O 错误，排空后检查，避免在 `noexcept` 回调中抛异常。

This small example has no file rotation, timestamp formatting, crash recovery or automatic I/O retry. Those policies belong in a real logging application, outside the queue's data-transfer contract.
