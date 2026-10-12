# WarmPool 用例

| 用例 | 操作和校验 |
|---|---|
| protocol | 编译期拒绝缺接口、错误返回值、非 noexcept；验证 DefaultResource 的 finish；接受只实现 reset 的独立资源 |
| resource | 单线程/并发 DefaultResource 的 reset，以及测试资源自定义的完成状态与代号访问 |
| lifecycle | min/max、耗尽、乱序完成、代号更新、每轮回收一次、查询不触发回收 |
| independent | 没有基类或任何 Block 字段的资源可使用 |
| failures | 创建抛异常/空指针，reset 返回 false，部分准备后 reset 失败；失败后仍能借出 |
| factory_dispose | Factory 完整销毁对象且只执行一次，失败与析构无泄漏 |
| trim_destroy | dispose 并销毁空闲对象，缓存满时登记当前归还对象且满足 min_count |
| trim_recreate | 缩减后对象销毁，工厂重建失败时无容量泄漏，恢复创建后可以正常借出 |
| slow_hooks | 工厂、reset、dispose 和析构内查询池统计，证明不持池锁 |
| trim_overlap | 两次并发缩减的释放过程交错，仍不得突破 min_count 下限 |
| wait | 非正、负数、短超时及异步完成后的唤醒重试 |
| generation_limit | 最后一个有效代号可以借出，耗尽后不回绕复用 |
| concurrent | 多借用者、多归还者、显式归还和缩减交错校验编号与反码 |
| death_live | 未完成的资源阻止池析构 |
| header_only | 自定义示例资源与 warm_pool.hpp 编译运行，不依赖 Block 或组件链接库 |
| dispose_lifecycle | 回收与复用不 dispose；超过阈值登记待销毁；析构 dispose 剩余资源 |
| dispose_raw_memory | 原始 new[]/delete[] 资源，缩减后工厂重建再释放；reset 中部分准备失败回滚；已销毁对象不重复 dispose |
| dispose_ctor_failure | 工厂成功创建部分资源后抛异常，构造失败自动 dispose 已创建资源并销毁；借出 reset 失败登记待销毁 |

| duplicate_recycle | 多线程重复归还同一 Loan，只有一次成功 |
| stale_during_reset | 旧 Loan 与下一轮 reset 并行，不干扰新一代 |
| factory_loan | create(loan) 关联独立资源及 is_valid(resource) |
| default_finish | 无参 finish 归还、缩减销毁和旧 Loan 拒绝 |

| node_lifetime | Loan 副本共享锁，旧代失效，串行策略可用 |
| node_concurrent | 旧节点锁未退出时 get 不 reset，且不占用池锁 |
| deferred_two_candidates | 锁忙旧记录重排；每轮尝试两个不同记录，析构清空尾部 |
| retired_record_reuse | 逻辑容量立即可用，旧记录持有独立对象指针，不误销毁新对象 |
