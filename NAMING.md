# Naming conventions

[English](NAMING.md) · [简体中文](NAMING.zh-CN.md)

- Classes, structs, enums and custom type aliases use `PascalCase`, such as `WriterQueue`, `DataFrame` and `ReadConfig`. Preserve existing compliant class names.
- Functions and members use `snake_case`, such as `allocate()`, `try_read()` and `add_writer()`. Constructors, destructors and operators keep their C++ language-defined names.
- Private/internal state members use `snake_case_`, such as `write_ptr_` and `current_range_`.
- Public fields in configuration, statistics and record structs use `snake_case` without a trailing underscore, including internal nested record structs.
- Ordinary variables and constants use `snake_case`.
- Template parameters use `T` followed by `PascalCase`: `TResource`, `TBlock`, `TIsConcurrent`, `TMaxCount`. This applies to type and non-type parameters.
- Macros use `UPPER_SNAKE_CASE`. Preserve standard-library protocol names such as Clock's `rep`, `period`, `duration`, `time_point` and `is_steady`, and preserve OS/third-party API names.

## 1. Historical API-name migration

This migration standardized names without retaining uppercase function aliases. It did not change message layout, ownership, synchronization or error handling; external callers needed to update their spelling.

| Previous name | Current name |
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

Public `Entry::finishMem/nextMem` fields became `finish_mem/next_mem`; `BlockPool::kBlockAlignment` became `block_alignment`. To avoid collisions, the internal `QueueReader::poll()` became `poll_once()`, and ReadService's registration template became `register_writer()`.

Review reports and recorded performance/test results are historical evidence. Their old symbols are not current API examples. Current runnable examples, tests and benchmark sources track the actual interfaces.

## 2. Historical source-block naming

`MemRange` was renamed `WriterBlock` during the source-block migration; the historical files were `src/write/writer_block.hpp` and `.cpp`. The state enum was `SourceBlockState`, and writers exposed their source through `source_block()`. That class derived from `ReaderBlock<>`, adding publication/delivery cursors and writer-completion state without changing block lifetimes.

The old source-block manager was removed. Writer references SpscLiveHandoffQueue directly and exposes it through `handoff_queue()`. `WriterPool` and `WriterQueue` are aliases for existing components rather than new manager classes. Use current headers and the [API guide](docs/guides/API.md) for today's whole-block source types.

## 3. Block resource factory naming

`MemoryResourceFactory` became `BlockResourceFactory`, in `src/base_components/block_slice/block_resource_factory.hpp`. It creates Blocks without owning their return policy, matching WarmPool resource terminology. Forward declarations, friends, SlicePool's Factory alias, test helpers and documentation changed together; constructor signatures stayed the same.

## 4. Template parameter naming

Use `T` plus `PascalCase` for both type and value parameters: `TResource`, `TBlock`, `TIsConcurrent`, `TMaxCount`. The prefix indicates a template parameter, not whether it is a type or a value.

WarmPool's resource parameter changed from `T` to `TResource`, and its locking-policy parameter from `is_n_alloc` to `TIsConcurrent`. The old name originated in BasicBlockSlicesAllocator, paired with `is_n_use` to mean multiple allocators/releasers. In the pool it instead selects `Lock = std::conditional_t<TIsConcurrent, SpinLock, NoLock>` for all pool operations: borrowing, returning, protected access, preallocation, counts and callback registration. The new name reflects that broader scope. Both parameters are positional, so call sites did not change.

BasicBlock, ReaderBlock, WriterBlock and BasicBlockSlicesAllocator retain the existing `is_n_alloc` parameter. Their second parameter is `is_n_use`, covering multithreaded block use, slice release and forced recycling. Either true enables cross-thread metadata and Node locking. These legacy parameters have not yet adopted the `T` prefix.
