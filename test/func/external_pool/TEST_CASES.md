# 外部池用例

| 用例 | 操作 | 预期 |
|---|---|---|
| null_single / null_1n / null_nn | 对三种 SlicePool 传 nullptr | 抛 invalid_argument，无析构副作用 |
| lifetime_single / lifetime_1n / lifetime_nn | 析构适配器但保留切片；访问并释放后重新借出块 | 不销毁外部池、不覆盖数据、可继续复用 |
| shared_single / shared_1n / shared_nn | 两个适配器分享池；池耗尽；关闭一个；释放复用；另一个继续分配 | current_ 不重叠、关闭相互独立、池容量和统计共享 |
| min_count | min_count=0/非零、max 小于 min、缩减总量下限 | 初始化数量正确、非法参数拒绝、总数量保持下限 |
| callback | 外部池安装释放回调，适配器退出后继续触发回收 | 回调仍生效且没有指向适配器的内部回调 |
| concurrent_1n / concurrent_nn | 四个共享适配器、直接池取块者、扫描者与多个释放者并发，校验编号和反码 | 无重复、丢失、覆盖、死锁或池容器竞争 |

并发用例设置有界总块数和缩减阈值，使取块、复用、归还、缩减发生交错；NN 用例同一适配器另有两个线程并发分配。
