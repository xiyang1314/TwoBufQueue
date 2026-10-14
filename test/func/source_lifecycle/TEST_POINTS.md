# 快速源块生命周期

源路径使用 SpscWriteBlock，不分配 Slice。旧 Slice 生命周期测试由 block_lifecycle、block_roles、block_templates 保留。
源测试改用 SpscLiveHandoffQueue 的 current/try_claim/finish，不再通过 read/read_done 提前移走记录。

- 写完后首次读取、先读 current 后读尾部：检查完整字节及单次交付。
- current 已全部 consume 仍不能归还，必须等完成记录 finish；空块不调用业务回调。
- 单块池在 Claim 未完成时不能复用，finish 后同地址新代可以继续写。
- 回调执行期间块仍被占用；true 后才允许复用；false 后整批保持可重试。
- 慢退超时后重启继续原批；取消领取不改变队首。
- 并发发布尾部/完成与 current 读取，400 代循环校验无漏读或重复。
- 多个空/非空完成块穿插，统计准确；同块 1300 次增量读取不依赖 Slice 槽位。
- 下游保存背压在没有释放通知时仍定时重试；停机可丢弃未读尾部。
- test/handoff/live_handoff_tests.cpp 另覆盖重复 acquire、取消与过期凭证、独立容量/池上限、工厂/reset 失败、
  on_send 后入队前的暂停窗口、跨线程 FIFO，以及多 Writer 整批 current 拒绝期间追加数据。

原先“注入任意 SourceBlockMgr::Entry”和“保留源 Slice 至池析构”的测试不再适用于新公开接口；
队列只能通过合法 ProducerLoan 发布，源回调不转移所有权。
