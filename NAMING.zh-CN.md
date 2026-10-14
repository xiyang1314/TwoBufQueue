# 命名规范

[English](https://xiyang1314.github.io/TwoBufQueue/NAMING.html) · [简体中文](https://xiyang1314.github.io/TwoBufQueue/NAMING.zh-CN.html)

- 类、结构体、枚举类型、自定义类型别名：`PascalCase`，例如 `WriterQueue`、`DataFrame`、`ReadConfig`。现有类名已符合，保持原名。
- 函数与成员函数：`snake_case`，例如 `allocate()`、`try_read()`、`add_writer()`。构造/析构函数遵循 C++ 规则，与类同名；运算符保留语言规定名称。
- 类的内部状态成员：`snake_case_`，例如 `write_ptr_`、`current_range_`。
- 配置、统计、记录等结构体的公开数据字段：`snake_case`，例如 `block_size`、`frames`、`finish_mem`；不使用尾下划线。内部嵌套记录结构体也遵循此规则。
- 普通变量和常量：`snake_case`。
- 模板参数统一使用 `T` 前缀加 `PascalCase`：类型模板参数如 `TResource`、`TBlock`；值模板参数（`bool` 及其他非类型参数）如 `TIsConcurrent`、`TMaxCount`。详见「模板参数命名」。
- 宏保持 `UPPER_SNAKE_CASE`。标准库协议名称（如 Clock 的 `rep`、`period`、`duration`、`time_point`、`is_steady`）和系统/第三方 API 不改名。

## 一、本次 API 迁移

本次只统一命名，不保留大写函数别名，不改变消息布局、所有权、同步或错误处理。外部调用需要同步更新。

| 原名称 | 新名称 |
|---|---|
| `AddWriter()` | `add_writer()` |
| `Allocate()` | `allocate()` |
| `Close()` | `close()` |
| `Finish()` | `finish()` |
| `Flush()` | `flush()` |
| `Get()` | `get()` |
| `MakeSlice()` | `make_slice()` |
| `Notify()` | `notify()` |
| `NotifyMemoryAvailable()` | `notify_memory_available()` |
| `Poll()` | `poll()` |
| `Preallocate()` | `preallocate()` |
| `ReadAvailable()` | `read_available()` |
| `Release()` | `release()` |
| `ReportInvalid()` | `report_invalid()` |
| `RequestStop()` | `request_stop()` |
| `Reset()` | `reset()` |
| `Save()` | `save()` |
| `Seal()` | `to_complete()` |
| `Set()` | `set()` |
| `SetBatchAvailableCallback()` | `set_batch_available_callback()` |
| `SetErrorHandler()` | `set_error_handler()` |
| `SetReadAvailableCallback()` | `set_read_available_callback()` |
| `SetReleaseCallback()` | `set_release_callback()` |
| `SetSpaceAvailableCallback()` | `set_space_available_callback()` |
| `Start()` | `start()` |
| `Stop()` | `stop()` |
| `TryRead()` | `try_read()` |
| `TryWait()` | `try_wait()` |
| `Wait()` | `wait()` |
| `WaitFor()` | `wait_for()` |
| `WaitUntil()` | `wait_until()` |

公开字段 `Entry::finishMem/nextMem` 改为 `finish_mem/next_mem`；`BlockPool::kBlockAlignment` 改为 `block_alignment`。
为避免重名，`QueueReader` 的内部 `poll()` 改为 `poll_once()`，`ReadService` 的内部注册模板改为 `register_writer()`。

审查报告及既有性能/测试结果是历史记录，其中的旧符号不作为当前 API 示例；当前示例、测试源码与基准源码随接口更新。

## 二、源数据块命名

`MemRange` 改为 `WriterBlock`，对应文件为 `src/write/writer_block.hpp` 和 `.cpp`；
状态枚举为 `SourceBlockState`，写入器通过 `source_block()` 取得当前源块。
该类继承 `ReaderBlock<>`，增加源端发布/交付游标和写入完成状态；命名调整不改变块的生命周期。

旧源块管理器已删除。Writer 直接引用 SpscLiveHandoffQueue，通过 handoff_queue() 提供给读端；
WriterPool/WriterQueue 仅是现有组件的类型别名，不是新增管理类。

## 三、块资源工厂命名

`MemoryResourceFactory` 改为 `BlockResourceFactory`，对应文件为 `src/base_components/block_slice/block_resource_factory.hpp`；
该类只创建 Block，不负责内存归还，名称与 `WarmPool` 的资源语义对齐。
前置声明、友元声明、`SlicePool` 的 `Factory` 别名、测试辅助类及文档同步改名，构造签名不变。

## 四、模板参数命名

模板参数统一使用 `T` 前缀加 `PascalCase`，类型参数和值参数（`bool` 及其他非类型参数）均适用：
`TResource`、`TBlock`、`TIsConcurrent`、`TMaxCount`。`T` 表示模板参数，不区分类型与值。

`WarmPool` 的资源类型参数由 `T` 改为 `TResource`；池锁策略参数由 `is_n_alloc` 改为 `TIsConcurrent`。
后者原名沿自 `BasicBlockSlicesAllocator`，在分配器中与 `is_n_use` 成对表示「N 个分配者 / N 个释放者」，
但该参数在池内只决定 `Lock = std::conditional_t<TIsConcurrent, SpinLock, NoLock>`，覆盖全部池操作（借出、归还、
受保护访问、预分配、计数查询、回调注册），与「分配」无对应关系，故改名。两个参数均为位置传参，调用点无需修改。

`BasicBlock`、`ReaderBlock`、`WriterBlock` 与 `BasicBlockSlicesAllocator` 保留 `is_n_alloc`，
第二个参数已按接口约定改为 `is_n_use`，表示块由多个线程使用，包括切片释放和强制回收。
两个参数任一为 true 时启用跨线程元数据及 Node 锁；此处暂未统一成 T 前缀命名。
