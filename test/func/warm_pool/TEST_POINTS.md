# WarmPool 测试点

1. Resource 只要求 noexcept reset(generation) -> bool 及不抛异常的析构；Factory::dispose(Resource*) 完整销毁对象。
2. DefaultResource 每次借出绑定 Loan，finish() 无参数归还；独立资源不要求继承。
3. 同代归还在池 SpinLock 内认领，复制 Loan、多线程重复归还只有一次成功。
4. 旧 Loan 在对象销毁、Node 复用、下一代 reset 期间仍可校验，且不能归还新一代。
5. 创建、reset、Factory::dispose 在池锁外；构造和 reset 失败回滚容量与所有权。
6. LIFO 缓存、max_free_count、min_count 优先及并发缩减下限。
7. Node 锁不占用池锁；已进入的旧操作退出前不能 reset 同一节点，新旧 Loan 共享节点锁。
8. 登记销毁前至多尝试两个不同旧记录；锁忙放回队尾，新记录最后入队。
9. 队列保存旧对象指针，max_count=1 时 Node 复用不会误销毁新对象；析构清理尾部记录。
10. 多生产者/消费者传递编号与反码、超时等待、代号耗尽和未归还时析构诊断。

Block/Slice 的实际销毁、旧凭证和 force_recycle 同步另由 block_lifecycle 的 trim 测试覆盖。
is_valid 是瞬时检查；通过可能失效的 Loan 访问资源时，由使用者先取得 Node 锁，再校验、访问。
