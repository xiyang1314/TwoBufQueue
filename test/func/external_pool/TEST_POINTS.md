# 外部 BlockPool 与 min_count 测试点

1. min_count 同时表示构造预分配数量和空闲缩减后的总块数下限；更名不改变原有行为。
2. 三种 SlicePool 均继承外部池指针构造函数；空指针构造抛 invalid_argument。
3. SlicePool 不拥有传入的池，析构只关闭自身当前块；已有切片可继续使用和释放，外部池继续可用。
4. 外部池内多个 SlicePool 的 current_、关闭状态和块归属互不干扰；关闭其中一个不能关闭其他实例。
5. 外部池模式取块使用公共池锁，覆盖多个 SlicePool、直接 BlockPool::get 和回收扫描并发，检查数据完整性。
6. 内部独占模式保持原来的分配锁和无锁取块路径；现有功能、BigMemoryStore、缩减及并发回归仍通过。
7. block_count、in_use_count、all_released 和释放回调仍属于整个底层池；回调由外部所有者管理生命周期，SlicePool 析构不清除它。
