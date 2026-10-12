# 测试用例

| 用例 | 操作 | 期望 |
|---|---|---|
| interfaces | 编译期检查三类块的公开 API 和共同 Slice 类型 | 基础类无业务分配，Writer 无普通 allocate，Reader 无读取发布接口；共用 Slice |
| mixed_batch | 从两个独立池取得 Writer 和 Reader 切片放入同一批，移交线程后逆序释放 | 数据完整，归还各自的池，释放一个池不影响另一个池 |
| reader_single / reader_1n / reader_nn | 从空块分配、对齐填充、超容量及槽位请求、结束分配和复用 | 游标与数据保持正确，当前块不提前回收，复用从零开始 |
| writer_publish | 写入但不发布、逐段发布、空增量读取、结束和出队后读取尾部 | 只暴露已发布数据；未读或未释放时不能回收 |
| writer_trim_restore | 空闲阈值设零，回收后重新取得描述符并发布新内容 | 缓冲区和发布指针重新绑定，无旧数据或旧游标 |
| writer_scan_release | 单线程写入、多轮读取、异步释放、另一线程扫描池 | 不出现未释放数据被覆盖，最终恰好回收一次 |
| reader_finish_with_held_slice | 结束分配后仍在写入已持有的 slice，再发布给消费者 | 持有的 slice 阻止回收，消费者得到完整内容 |

集成验证：完整 Windows CTest；Linux ThreadSanitizer 并发/生命周期/角色拆分套件；AddressSanitizer 与 UndefinedBehaviorSanitizer 生命周期/角色拆分套件。

旧的显式 make_slice(offset, size) 已从生产接口移除：重复范围死亡测试由编译期禁止传范围、重复无增量读取不占槽位测试替代；越界范围测试改为拒绝非法发布游标。
