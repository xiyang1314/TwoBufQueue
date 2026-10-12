#pragma once

namespace TwoBufQueue {

// 用于已由调用方保证串行访问的路径：单一执行流，或同一外层锁保护。
// 块池的取块与扫描共用该串行约束；切片分配与释放的并发策略由分配器独立决定。
struct NoLock {
    void lock() noexcept {}
    bool try_lock() noexcept { return true; }
    void unlock() noexcept {}
};

} // namespace TwoBufQueue
