# 块池与完整管道接入测试点

先核对下面的契约，再实现对应测试，保留原有数据完整性断言。

| 测试点 | 用例与预期 |
|---|---|
| 直接使用资源池 | SlicePool 直接持有 WarmPool；Writer 通过 SpscLiveHandoffQueue 使用外部 WarmPool；BlockResourceFactory 仅创建 Block，Slice 保存稳定节点凭证，BlockDiagnostics 提供静态诊断；无旧包装类和头文件引用 |
| 唯一写游标 | WriterBlock 独有 write_ptr_，read_pos() 来自公共分配器；ReaderBlock 使用 allocated_size()；各种长度前缀、批量 flush、对齐、容量边界和多代复用保持正确 |
| 交接状态 | 发布完成记录前为 WriteFinished；记录出队后为 LeaveWriter；WriteFinished 即使无活跃切片也不回收 |
| 未读尾部 | LeaveWriter 仍有未读字节时不可回收；读取尾部、最后一次释放后才满足回收条件 |
| 完成归还 | 最后一个切片释放或状态交接使块满足条件时立即归还；get 只借用，不扫描，不丢块、不重复借出 |
| 空块 | 无数据的活动读取不分配占位；完成交接后可回收；与非空块穿插无空回调 |
| 排空与析构 | 所有帧已释放且交接完成时能判断排空；销毁时可丢弃未消费尾部，不能销毁仍由用户持有的帧 |
| 重试 | 源池耗尽、存储块被用户保留、批次队列满；无需切片释放通知也能在限定重试间隔后恢复，拒绝回调保持原批次 |
| 并发 | 最后一次释放、LeaveWriter 发布、归还和复用交错；多取块者、多释放者、跨位图字边界，验证序号、校验值及完整字节 |
| 强制回收 | WriterBlock 仅 LeaveWriter、ReaderBlock 仅 AllocFinished 能标记 ForceRecycle，旧切片失效且不能改动新代计数；Free/ForceRecycle 不允许 allocate_read_slice |

对应实现：本目录的底层回归、source_lifecycle 交接回归，以及 func、concurrent 的完整直读和两层管道用例。
Windows 全量构建和 CTest；Linux 分别启用 ThreadSanitizer、AddressSanitizer/UndefinedBehaviorSanitizer。
