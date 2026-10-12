# 公共 Block 模板与独立状态测试点

1. BasicBlock 不含业务状态、状态名称或状态转换；ReaderBlock 和 WriterBlock 的 State 是不同枚举类型。
2. 三类块使用 bool is_n_alloc / bool is_n_use；相同策略的两种块共用切片类型和 BasicBlockSlicesAllocator。
3. WriterBlock 在 base_components/block_slice 目录提供完整模板定义，可以在独立翻译单元中实例化四种参数组合，不依赖 write 目录。
4. is_n_alloc=true 时，WriterBlock 的发布位置快照与切片登记统一加锁，多分配者不重复或遗漏数据；is_n_use=false 时只有一个释放者，但它可以与多个分配者并行。
5. false/false 为同一执行流或完全外部串行场景，不使用内部原子及锁；其余策略保护跨线程元数据。
6. 正常回收、强制回收和缩减复用正确使旧代号失效；分配器不依赖任何派生状态。
7. 错误报告按实际块类型输出状态名称，包括无池归属、Free、强制失效和跨代切片。

保留直接读取、BigMemoryStore、并发、生命周期及缩减回归；本次修改不提交 Git。
