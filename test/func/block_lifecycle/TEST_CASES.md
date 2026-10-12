# 底层块生命周期用例

测试入口为本目录的 tests.cpp 和 CMakeLists.txt，CTest 名称使用 `block_lifecycle_` 前缀。
三种策略分别独立实例化，不通过编译开关切换全局模式。

| 用例 | 步骤与预期 |
|---|---|
| state_single、state_1n、state_nn | Writing 下分配及释放，WriteFinished 仍不能回收；LeaveWriter 后下一次 get 自动扫描、重置并复用原地址 |
| read_progress | 发布 32 字节，只读取并释放前 16 字节；交出块后仍不能复用，尾部登记和释放后才可回收 |
| force_ignore | 非严格释放检查：强制回收后及复用后释放旧句柄，不修改新代位图、计数或数据 |
| scan_skip_head | 四块混合就绪，跳过仍持有的队首和队中块；只回收已释放块，余下负载逐字节保持不变 |
| scan_before_create | 存在可回收块时优先复用，再考虑新建；静止时预分配仍遵守容量上限 |
| bounds_single、bounds_1n、bounds_nn | 对齐补齐、恰好耗尽字节、槽位耗尽和失败请求；失败不推进游标，释放不复用同代编号 |
| bitmap_reuse | 1300 个切片跨多字位图边界，多释放线程、八代复用，逐条核对负载和代号 |
| last_release_scan | 最后一次释放与 get/扫描同时开始，立即复用并写入新内容；旧释放完成后新代计数及字节仍正确 |
| many_getters | 四个取块者、四个释放者，共 6000 条；独立序号与反码验证无缺失、重复或覆盖 |
| same_block_alloc_release | 直接多线程共享一个块分配并释放；Writing 期间反复扫描不得提前回收 |
| wait_timeouts | 零、负时限只尝试一次；正时限有界；释放后在剩余期限内通过重扫取得块 |
| wait_multiple | 四个等待者取得四个不同块；取得后保留到全部等待者成功，避免一个块被依次复用伪造通过 |
| slice_pool_single、slice_pool_1n、slice_pool_nn | 三类 SlicePool 反复自动换块、复用；close 幂等且禁止继续分配 |
| close_then_release | 先 close，后由其他线程释放现有切片；析构扫描可安全完成 |
| notifications | release 不通知；实际扫描回收后按块通知，回调在 BlockPool 分配锁之外执行 |
| destructor_scan | 没有后续 get 的情况下，析构清理已经满足条件的块，并且不调用外部回调 |
| pointer_single、pointer_1n、pointer_nn | 未初始化偏移为零；初始及复用指针指向 buf_；对齐、容量末端、失败分配和五代重置正确 |
| pointer_publication | 写线程先写入序号及反码，再发布写指针；读取线程登记并校验 10000 条记录 |
| slice_state_range | Writing、WriteFinished、LeaveWriter 各登记连续切片；静态断言 Free/ForceRecycle 小于 Writing |
| intrusive_queue | 空队列、单节点、FIFO、栈与队列转移，20000 次固定种子混合操作与独立 deque 参照一致 |
| derived_recycle_condition | 使用队列的 StorageBlock 链接不绕过具体 Block 的自定义回收条件 |

以下用例运行在独立子进程中，要求专用退出码 91 且错误文字匹配；普通崩溃、超时和错误原因不符均失败。

| death 用例 | 应拒绝的操作 |
|---|---|
| force_writing、force_finished、force_free、force_twice | 在 LeaveWriter 以外的状态调用 force_recycle |
| leave_writing、leave_twice、finish_twice | 跳过或重复状态转换 |
| allocate_finished | 完成写入后继续普通 allocate |
| slice_bounds、slice_duplicate | 登记超出已发布字节的范围，或重复登记非连续范围 |
| slice_free、slice_forced | 在 Free、ForceRecycle 状态登记切片 |
| release_twice、release_forced、release_stale | 严格模式下重复释放、释放强制失效句柄或旧代句柄 |
| destructor_writing、destructor_unread、destructor_slice | 块仍在写入、尚有未读字节、或用户仍持有切片时销毁池 |

完整管道的补充用例位于相邻 source_lifecycle 目录：

- `buffered_retry_without_release_notify`：活动扫描间隔设为 10 秒，不发送释放唤醒；保留第一批帧阻止第二批保存，释放后须在 2 秒内交付第二批，内容和计数准确。
- `discard_unread_tail`：停机时仍有未交付尾部，管理器通过临时切片清理，不遗留无法回收的读取进度。
- `held_frame_destruction`：用户已取得但未释放的帧不能被析构清理隐式作废，必须报告契约错误。
