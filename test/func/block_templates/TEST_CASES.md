# 公共 Block 模板测试用例

| 用例 | 操作 | 期望 |
|---|---|---|
| interfaces | 编译期检查基础类与派生类接口、状态类型、返回切片类型 | 基础类无业务状态；两个派生状态不同；同一策略 Slice 相同 |
| writer_00 / writer_01 / writer_10 / writer_11 | 分配块、发布、增量登记、乱序释放、结束、复用 | 四种组合正确，生命周期不提前回收 |
| reader_10 | 多线程直接从同一 ReaderBlock 分配，单线程消费释放 | 地址不重叠，数据不丢失，不因 release=false 产生数据竞争 |
| writer_parallel_10 / writer_parallel_11 | 一个发布者、多个切片分配者、一个/多个释放者和池扫描并行 | 字节范围不重叠、完整覆盖；每条记录内容及校验值一致 |
| force_writer / force_reader | 标记强制回收、释放旧句柄、扫描、同址复用、再次释放旧代 | 宽容模式忽略失效释放，不修改新代计数；严格模式由已有生命周期死亡用例覆盖 |
| death_writer_state / death_reader_state | 触发非法状态转换并检查 ErrorInfo | 实际状态值及名称来自对应块类型 |
| header_only | 只包含 base_components/block_slice/writer_block.hpp 并实例化全部模板组合 | 无旧 cpp 链接依赖 |

并行分配测试还覆盖空增量、全部槽位耗尽前恰好覆盖总字节数、快照读取与范围分配交错。
