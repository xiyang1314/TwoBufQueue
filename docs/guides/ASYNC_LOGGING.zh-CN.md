# 异步日志示例怎么使用

生产者把日志内容提交给队列，后台回调负责写文件。这让业务线程不用在每次记日志时等待文件操作。示例会校验最终文件中每条已接受记录的内容、顺序和数量。

[示例源码](../../examples/two_buf_queue/09_async_logger.cpp) · [接口指南](API.zh-CN.md) · [English explanation](ASYNC_LOGGING.zh-CN.md)

## 一、构建并运行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target brq_tbq_09_async_logger
./build/brq_tbq_09_async_logger ./application.log
```

Visual Studio 构建使用 `build/Release/brq_tbq_09_async_logger.exe`。程序会覆盖选定日志文件；不传路径时，默认写当前目录下的 `twobufqueue-example.log`。

## 二、三种运行方式

| 命令参数 | 用来观察什么 |
|---|---|
| 不加额外参数 | 正常发送和文件保存 |
| `--slow-sink` | 第一次回调暂停 20 ms，模拟消费变慢 |
| `--slow-sink --drop` | 消费变慢时拒绝新消息，并统计被拒绝条数 |

```sh
./build/brq_tbq_09_async_logger ./slow.log --slow-sink
./build/brq_tbq_09_async_logger ./drop.log --slow-sink --drop
```

示例默认使用 Block，缓存满时最多等待 2 秒；仍失败则记录拒绝并以非零结束。Drop 模式立即拒绝本次消息并继续，拒绝的消息不会被算作已经保存。具体压力次数受线程调度影响。

为了容易观察缓存满，示例两层各设两个 4 KiB 块。这个配置用于演示行为，不是最快参数或纯写入性能测试。

## 三、flush 与真正写完文件的区别

1. `writer.flush()` 发布已写消息并请求后台读取，不等待文件回调。
2. `writer.finish()` 结束发送并交接最后一块。
3. `queue.stop()` 收尾后台处理，再检查队列错误。
4. 应用再执行文件 flush、关闭并检查 I/O 错误。

关闭当前块扫描时，flush 不能代替尾块交接。文件 flush 也不等于断电后仍保证保存的持久化确认。逐条回调返回 void，文件错误由示例记录并在收尾时检查。

## 四、用在业务中还需补什么

文件轮转、时间戳格式、崩溃恢复、持久化与 I/O 重试属于日志系统策略，示例没有自动实现。回调接收到的结构体可能没有对齐，先检查长度，再复制到本地对象。更完整的记录与所有权约定见[接口指南](API.zh-CN.md)。
