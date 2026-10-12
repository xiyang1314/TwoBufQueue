# 功能测试用例

测试点见 TEST_POINTS.md。cases.json 为机器可读清单。每个ID独立CTest；不得将一组参数通过当成整个矩阵通过。

|ID|测试点|前置条件|操作|期望|
|---|---|---|---|---|
|W_len_0|W1 消息协议及边界|块恰好容纳0+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_1|W1 消息协议及边界|块恰好容纳1+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_2|W1 消息协议及边界|块恰好容纳2+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_3|W1 消息协议及边界|块恰好容纳3+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_4|W1 消息协议及边界|块恰好容纳4+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_31|W1 消息协议及边界|块恰好容纳31+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_32|W1 消息协议及边界|块恰好容纳32+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_255|W1 消息协议及边界|块恰好容纳255+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_256|W1 消息协议及边界|块恰好容纳256+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_1023|W1 消息协议及边界|块恰好容纳1023+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_1024|W1 消息协议及边界|块恰好容纳1024+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_65534|W1 消息协议及边界|块恰好容纳65534+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_len_65535|W1 消息协议及边界|块恰好容纳65535+2字节|写确定性字节序列；修改输入；Finish；排空|只收到1条；两字节小端前缀和每字节与原副本相同；池归零|
|W_reject_null|W2 非法长度无副作用|null|先写合法值；提交非法请求；核对游标；再写合法值并排空|非法请求false；原游标不变；只收到两条合法记录|
|W_reject_oversize|W2 非法长度无副作用|oversize|先写合法值；提交非法请求；核对游标；再写合法值并排空|非法请求false；原游标不变；只收到两条合法记录|
|W_short_payload|W2 短消息无需最小长度配置|3字节负载|写入3字节消息并排空|成功且完整收到3字节负载|
|W_reject_block|W2 非法长度无副作用|block|先写合法值；提交非法请求；核对游标；再写合法值并排空|非法请求false；原游标不变；只收到两条合法记录|
|W_publish_1|W3 发布可见性|发布次数1|写满一批再写尾部；检查发布游标；Flush；Finish|未发布尾部不可见；Flush发布全部；字节顺序正确|
|W_publish_2|W3 发布可见性|发布次数2|写满一批再写尾部；检查发布游标；Flush；Finish|未发布尾部不可见；Flush发布全部；字节顺序正确|
|W_publish_32|W3 发布可见性|发布次数32|写满一批再写尾部；检查发布游标；Flush；Finish|未发布尾部不可见；Flush发布全部；字节顺序正确|
|W_pack|W4 模板及幂等结束|pack|多参数紧密布局/Finish两次及结束后写入|布局与各字段对象表示拼接一致；结束后拒写；无重复交付|
|W_finish|W4 模板及幂等结束|finish|多参数紧密布局/Finish两次及结束后写入|布局与各字段对象表示拼接一致；结束后拒写；无重复交付|
|R_active|R1 活动/完成交接与所有权|active|手动ReadService推进；拒收/持有/取消/过期后重试|未接受不推进；重试不重复；持有期间字节稳定；最后释放归零|
|R_reject|R1 活动/完成交接与所有权|reject|手动ReadService推进；拒收/持有/取消/过期后重试|未接受不推进；重试不重复；持有期间字节稳定；最后释放归零|
|R_retain|R1 活动/完成交接与所有权|retain|手动ReadService推进；拒收/持有/取消/过期后重试|未接受不推进；重试不重复；持有期间字节稳定；最后释放归零|
|R_cancel|R1 活动/完成交接与所有权|cancel|手动ReadService推进；拒收/持有/取消/过期后重试|未接受不推进；重试不重复；持有期间字节稳定；最后释放归零|
|R_deadline|R1 活动/完成交接与所有权|deadline|手动ReadService推进；拒收/持有/取消/过期后重试|未接受不推进；重试不重复；持有期间字节稳定；最后释放归零|
|R_reg_nullwriter|R2 注册校验|nullwriter|尝试无效注册后查询错误类别和writer_count|返回false；精确错误枚举；注册数量不增加|
|R_reg_nullqueue|R2 注册校验|空队列指针|调用 add_queue(nullptr, id)|返回 false 并记录 null_dependency|
|R_reg_queue_negative|R2 注册校验|有效队列与负数来源编号|调用 add_queue(&queue, -1)|返回 false 并记录 invalid_registration|
|R_reg_duplicate|R2 注册校验|duplicate|尝试无效注册后查询错误类别和writer_count|返回false；精确错误枚举；注册数量不增加|
|R_reg_duplicateid|R2 注册校验|duplicateid|尝试无效注册后查询错误类别和writer_count|返回false；精确错误枚举；注册数量不增加|
|R_reg_negative|R2 注册校验|negative|尝试无效注册后查询错误类别和writer_count|返回false；精确错误枚举；注册数量不增加|
|R_reg_running|R2 注册校验|running|尝试无效注册后查询错误类别和writer_count|返回false；精确错误枚举；注册数量不增加|
|R_config_scan0|R3 启动校验|scan0|无效配置Start|false；invalid_start_state；未启动线程|
|R_config_max0|R3 启动校验|max0|无效配置Start|false；invalid_start_state；未启动线程|
|R_config_finish0|R3 启动校验|finish0|无效配置Start|false；invalid_start_state；未启动线程|
|R_config_wait0|R3 启动校验|wait0|无效配置Start|false；invalid_start_state；未启动线程|
|R_config_retry0|R3 启动校验|retry0|无效配置Start|false；invalid_start_state；未启动线程|
|R_config_callback|R3 启动校验|callback|无效配置Start|false；invalid_start_state；未启动线程|
|R_config_writers|R3 启动校验|writers|无效配置Start|false；invalid_start_state；未启动线程|
|S_copy|S1 存储及消费|copy|保存多帧；检查processed连续前缀；读取/拒收/释放/重启|数据/来源/时间/统计一致；背压无丢失；保留句柄阻止回收|
|S_split|S1 存储及消费|split|保存多帧；检查processed连续前缀；读取/拒收/释放/重启|数据/来源/时间/统计一致；背压无丢失；保留句柄阻止回收|
|S_zero|S1 存储及消费|zero|保存多帧；检查processed连续前缀；读取/拒收/释放/重启|数据/来源/时间/统计一致；背压无丢失；保留句柄阻止回收|
|S_queuefull|S1 存储及消费|queuefull|保存多帧；检查processed连续前缀；读取/拒收/释放/重启|数据/来源/时间/统计一致；背压无丢失；保留句柄阻止回收|
|S_poolfull|S1 存储及消费|poolfull|保存多帧；检查processed连续前缀；读取/拒收/释放/重启|数据/来源/时间/统计一致；背压无丢失；保留句柄阻止回收|
|S_slots|S1 存储及消费|slots|保存多帧；检查processed连续前缀；读取/拒收/释放/重启|数据/来源/时间/统计一致；背压无丢失；保留句柄阻止回收|
|S_closed|S1 存储及消费|closed|保存多帧；检查processed连续前缀；读取/拒收/释放/重启|数据/来源/时间/统计一致；背压无丢失；保留句柄阻止回收|
|S_retry|S1 存储及消费|retry|保存多帧；检查processed连续前缀；读取/拒收/释放/重启|数据/来源/时间/统计一致；背压无丢失；保留句柄阻止回收|
|S_notify|S1 存储及消费|notify|保存多帧；检查processed连续前缀；读取/拒收/释放/重启|批次发布与出队各通知一次；帧释放不通知；finish 后 all_released 正确且查询无回收副作用|
|S_timeout|S1 存储及消费|timeout|保存多帧；检查processed连续前缀；读取/拒收/释放/重启|数据/来源/时间/统计一致；背压无丢失；保留句柄阻止回收|
|S_invalid_null|S2 输入全量预校验|null|混入无效DataView；调用Save|精确错误；processed=0；无部分数据入队|
|S_invalid_id|S2 输入全量预校验|id|混入无效DataView；调用Save|精确错误；processed=0；无部分数据入队|
|S_invalid_block|S2 输入全量预校验|block|混入无效DataView；调用Save|精确错误；processed=0；无部分数据入队|
|S_invalid_batch|S2 输入全量预校验|batch|混入无效DataView；调用Save|精确错误；processed=0；无部分数据入队|
|S_invalid_prevalidate|S2 输入全量预校验|prevalidate|混入无效DataView；调用Save|精确错误；processed=0；无部分数据入队|
|P_align_11_1|P1 三策略通用分配|模式11 对齐1|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_11_2|P1 三策略通用分配|模式11 对齐2|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_11_4|P1 三策略通用分配|模式11 对齐4|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_11_8|P1 三策略通用分配|模式11 对齐8|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_11_16|P1 三策略通用分配|模式11 对齐16|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_11_32|P1 三策略通用分配|模式11 对齐32|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_11_64|P1 三策略通用分配|模式11 对齐64|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_slots_11_1|P2 位图边界|模式11 槽位1|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_11_63|P2 位图边界|模式11 槽位63|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_11_64|P2 位图边界|模式11 槽位64|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_11_65|P2 位图边界|模式11 槽位65|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_11_129|P2 位图边界|模式11 槽位129|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_move_11|P3 唯一所有权|11|移动构造、覆盖移动赋值、自移动、析构和显式释放|源清空；旧目标释放；自移动保留；只回收一次|
|P_align_1N_1|P1 三策略通用分配|模式1N 对齐1|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_1N_2|P1 三策略通用分配|模式1N 对齐2|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_1N_4|P1 三策略通用分配|模式1N 对齐4|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_1N_8|P1 三策略通用分配|模式1N 对齐8|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_1N_16|P1 三策略通用分配|模式1N 对齐16|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_1N_32|P1 三策略通用分配|模式1N 对齐32|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_1N_64|P1 三策略通用分配|模式1N 对齐64|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_slots_1N_1|P2 位图边界|模式1N 槽位1|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_1N_63|P2 位图边界|模式1N 槽位63|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_1N_64|P2 位图边界|模式1N 槽位64|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_1N_65|P2 位图边界|模式1N 槽位65|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_1N_129|P2 位图边界|模式1N 槽位129|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_move_1N|P3 唯一所有权|1N|移动构造、覆盖移动赋值、自移动、析构和显式释放|源清空；旧目标释放；自移动保留；只回收一次|
|P_align_NN_1|P1 三策略通用分配|模式NN 对齐1|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_NN_2|P1 三策略通用分配|模式NN 对齐2|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_NN_4|P1 三策略通用分配|模式NN 对齐4|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_NN_8|P1 三策略通用分配|模式NN 对齐8|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_NN_16|P1 三策略通用分配|模式NN 对齐16|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_NN_32|P1 三策略通用分配|模式NN 对齐32|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_align_NN_64|P1 三策略通用分配|模式NN 对齐64|先分配1字节；再分配对齐切片；超大请求；Close|地址对齐、不重叠；超大请求不破坏当前块；Close后拒绝分配|
|P_slots_NN_1|P2 位图边界|模式NN 槽位1|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_NN_63|P2 位图边界|模式NN 槽位63|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_NN_64|P2 位图边界|模式NN 槽位64|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_NN_65|P2 位图边界|模式NN 槽位65|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_slots_NN_129|P2 位图边界|模式NN 槽位129|分配至槽位上限；再申请失败；逆序释放；多代复用|失败不扩容；数据完整；归零；复用成功|
|P_move_NN|P3 唯一所有权|NN|移动构造、覆盖移动赋值、自移动、析构和显式释放|源清空；旧目标释放；自移动保留；只回收一次|
|P_config_bytes|P4 构造参数|bytes|构造无效配置|抛出invalid_argument，不启动工作|
|P_config_max|P4 构造参数|max|构造无效配置|抛出invalid_argument，不启动工作|
|P_config_initial|P4 构造参数|initial|构造无效配置|抛出invalid_argument，不启动工作|
|P_config_slots|P4 构造参数|slots|构造无效配置|抛出invalid_argument，不启动工作|
|P_config_storequeue|P4 构造参数|storequeue|构造无效配置|抛出invalid_argument，不启动工作|
|C_direct_1_1|C1 并发端到端|direct；1写线程；发布1|同步起跑；每线程2001条混合长度与序号；重试背压；Finish后Stop|来源/逐生产者顺序/全部字节/总数一致；无残留；有界结束|
|C_direct_1_32|C1 并发端到端|direct；1写线程；发布32|同步起跑；每线程2001条混合长度与序号；重试背压；Finish后Stop|来源/逐生产者顺序/全部字节/总数一致；无残留；有界结束|
|C_direct_4_1|C1 并发端到端|direct；4写线程；发布1|同步起跑；每线程2001条混合长度与序号；重试背压；Finish后Stop|来源/逐生产者顺序/全部字节/总数一致；无残留；有界结束|
|C_direct_4_32|C1 并发端到端|direct；4写线程；发布32|同步起跑；每线程2001条混合长度与序号；重试背压；Finish后Stop|来源/逐生产者顺序/全部字节/总数一致；无残留；有界结束|
|C_buffered_1_1|C1 并发端到端|buffered；1写线程；发布1|同步起跑；每线程2001条混合长度与序号；重试背压；Finish后Stop|来源/逐生产者顺序/全部字节/总数一致；无残留；有界结束|
|C_buffered_1_32|C1 并发端到端|buffered；1写线程；发布32|同步起跑；每线程2001条混合长度与序号；重试背压；Finish后Stop|来源/逐生产者顺序/全部字节/总数一致；无残留；有界结束|
|C_buffered_4_1|C1 并发端到端|buffered；4写线程；发布1|同步起跑；每线程2001条混合长度与序号；重试背压；Finish后Stop|来源/逐生产者顺序/全部字节/总数一致；无残留；有界结束|
|C_buffered_4_32|C1 并发端到端|buffered；4写线程；发布32|同步起跑；每线程2001条混合长度与序号；重试背压；Finish后Stop|来源/逐生产者顺序/全部字节/总数一致；无残留；有界结束|
|C_seal1N|C2 并发释放与回收|seal1N|barrier控制Seal/Release竞争或多线程分配；重复多轮|活跃地址不重叠；数据不串写；每块恰回收一次|
|C_sealNN|C2 并发释放与回收|sealNN|barrier控制Seal/Release竞争或多线程分配；重复多轮|活跃地址不重叠；数据不串写；每块恰回收一次|
|C_allocNN|C2 并发释放与回收|allocNN|barrier控制Seal/Release竞争或多线程分配；重复多轮|活跃地址不重叠；数据不串写；每块恰回收一次|
|C_release1N|C2 并发释放与回收|release1N|barrier控制Seal/Release竞争或多线程分配；重复多轮|活跃地址不重叠；数据不串写；每块恰回收一次|
|M_seed_1|M1 模型状态机|固定种子1；2块小池|1000步随机写/Flush/拒收/持有/释放/扫描；末尾排空|收到的完整消息序列等于成功写入的参考模型；池最终归零|
|M_seed_7|M1 模型状态机|固定种子7；2块小池|1000步随机写/Flush/拒收/持有/释放/扫描；末尾排空|收到的完整消息序列等于成功写入的参考模型；池最终归零|
|M_seed_42|M1 模型状态机|固定种子42；2块小池|1000步随机写/Flush/拒收/持有/释放/扫描；末尾排空|收到的完整消息序列等于成功写入的参考模型；池最终归零|
|M_seed_20260925|M1 模型状态机|固定种子20260925；2块小池|1000步随机写/Flush/拒收/持有/释放/扫描；末尾排空|收到的完整消息序列等于成功写入的参考模型；池最终归零|
|D_empty|D1 契约违规进程隔离|empty|子进程安装错误回调后触发指定违规|仅指定诊断原因触发哨兵退出码86；普通崩溃/其他异常不算通过|
|D_double|D1 契约违规进程隔离|double|子进程安装错误回调后触发指定违规|仅指定诊断原因触发哨兵退出码86；普通崩溃/其他异常不算通过|
|D_zero|D1 契约违规进程隔离|zero|子进程安装错误回调后触发指定违规|仅指定诊断原因触发哨兵退出码86；普通崩溃/其他异常不算通过|
|D_align3|D1 契约违规进程隔离|align3|子进程安装错误回调后触发指定违规|仅指定诊断原因触发哨兵退出码86；普通崩溃/其他异常不算通过|
|D_align128|D1 契约违规进程隔离|align128|子进程安装错误回调后触发指定违规|仅指定诊断原因触发哨兵退出码86；普通崩溃/其他异常不算通过|
|D_seal|D1 契约违规进程隔离|to_write_finished|子进程安装错误回调后触发指定违规|仅指定诊断原因触发哨兵退出码86；普通崩溃/其他异常不算通过|
|D_range|D1 契约违规进程隔离|range|子进程安装错误回调后触发指定违规|仅指定诊断原因触发哨兵退出码86；普通崩溃/其他异常不算通过|
|D_foreign|D1 契约违规进程隔离|foreign|子进程安装错误回调后触发指定违规|仅指定诊断原因触发哨兵退出码86；普通崩溃/其他异常不算通过|
|D_livepool|D1 契约违规进程隔离|livepool|子进程安装错误回调后触发指定违规|仅指定诊断原因触发哨兵退出码86；普通崩溃/其他异常不算通过|
|T_pointer|T1 编译期契约|void f(WriterQueue* p){Writer<> w(p,0);unsigned n=0;auto v=&n;(void)w.write(v);}|先编译合法对照，再编译违规模板实例|合法对照成功；违规实例失败且匹配预期static_assert诊断|
|T_array|T1 编译期契约|void f(WriterQueue* p){Writer<> w(p,0);char v[4]{};(void)w.write(v);}|先编译合法对照，再编译违规模板实例|合法对照成功；违规实例失败且匹配预期static_assert诊断|
|T_nontrivial|T1 编译期契约|void f(WriterQueue* p){Writer<> w(p,0);std::string v;(void)w.write(v);}|先编译合法对照，再编译违规模板实例|合法对照成功；违规实例失败且匹配预期static_assert诊断|
|T_maximum|T1 编译期契约|struct Large {char data[65536];};void f(WriterQueue* p){Writer<> w(p,0);Large v{};(void)w.write(v);}|先编译合法对照，再编译违规模板实例|合法对照成功；违规实例失败且匹配预期static_assert诊断|
|T_publish0|T1 编译期契约|void f(WriterQueue* p){Writer<0> w(p,0);}|先编译合法对照，再编译违规模板实例|合法对照成功；违规实例失败且匹配预期static_assert诊断|
|U_signal|U1 事件语义|signal|Set/TryWait/Reset/取消/无信号等待|信号保留一次；正确自动清除；取消退出；超时返回false|
|U_cancel|U1 事件语义|cancel|Set/TryWait/Reset/取消/无信号等待|信号保留一次；正确自动清除；取消退出；超时返回false|
|U_timeout|U1 事件语义|timeout|Set/TryWait/Reset/取消/无信号等待|信号保留一次；正确自动清除；取消退出；超时返回false|
|P_tolerant_11_old|P5 宽松释放契约|11；is_check_slice_release=false；old|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_11_zero|P5 宽松释放契约|11；is_check_slice_release=false；zero|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_11_free|P5 宽松释放契约|11；is_check_slice_release=false；free|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_11_index|P5 宽松释放契约|11；is_check_slice_release=false；index|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_11_duplicate|P5 宽松释放契约|11；is_check_slice_release=false；duplicate|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_1N_old|P5 宽松释放契约|1N；is_check_slice_release=false；old|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_1N_zero|P5 宽松释放契约|1N；is_check_slice_release=false；zero|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_1N_free|P5 宽松释放契约|1N；is_check_slice_release=false；free|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_1N_index|P5 宽松释放契约|1N；is_check_slice_release=false；index|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_1N_duplicate|P5 宽松释放契约|1N；is_check_slice_release=false；duplicate|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_NN_old|P5 宽松释放契约|NN；is_check_slice_release=false；old|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_NN_zero|P5 宽松释放契约|NN；is_check_slice_release=false；zero|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_NN_free|P5 宽松释放契约|NN；is_check_slice_release=false；free|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_NN_index|P5 宽松释放契约|NN；is_check_slice_release=false；index|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|P_tolerant_NN_duplicate|P5 宽松释放契约|NN；is_check_slice_release=false；duplicate|仅测试访问器注入失效代号/越界/重复释放；随后正常释放并复用|无错误；outstanding和池占用不变；合法释放仍恰回收一次|
|D_strict_old|D2 严格释放契约|old|注入失效内部释放请求|专用退出86且精确错误原因|
|D_strict_free|D2 严格释放契约|free|注入失效内部释放请求|专用退出86且精确错误原因|
|D_strict_index|D2 严格释放契约|index|注入失效内部释放请求|专用退出86且精确错误原因|
|D_strict_duplicate|D2 严格释放契约|duplicate|注入失效内部释放请求|专用退出86且精确错误原因|
|W_pressure_drop|W5 写端背压|drop|单块池耗尽；丢弃/总超时/解除拒收后释放后重试|失败记录未交付；恢复后顺序正确；blocked/dropped统计符合模式|
|W_pressure_block_timeout|W5 写端背压|block_timeout|单块池耗尽；丢弃/总超时/解除拒收后释放后重试|失败记录未交付；恢复后顺序正确；blocked/dropped统计符合模式|
|W_pressure_block_resume|W5 写端背压|block_resume|单块池耗尽；丢弃/总超时/解除拒收后释放后重试|失败记录未交付；恢复后顺序正确；blocked/dropped统计符合模式|
|R_stop_fast|R4 工作线程停止恢复|fast|拒收时停止；停止后允许接收；重启排空|有界退出；数据保留；重启恰好交付一次|
|R_stop_timeout|R4 工作线程停止恢复|timeout|拒收时停止；停止后允许接收；重启排空|有界退出；数据保留；重启恰好交付一次|
|S_config_callback|S3 QueueReader构造校验|callback|空回调/零重试间隔构造|invalid_argument|
|S_config_retry|S3 QueueReader构造校验|retry|空回调/零重试间隔构造|invalid_argument|
|M_complete_11|M2 统一物理块完成入口|11 策略；单块池；重复 3 代；保留独占切片|Writing 下分配、释放并继续分配；WriteFinished 保留切片；交出写入端后释放；显式扫描；覆盖空块和多代复用|Writing/WriteFinished 不提前回收；LeaveWriter 且全释放后可回收；扫描才减少占用并每块通知一次；每代内容正确|
|M_complete_1N|M2 统一物理块完成入口|1N 策略；单块池；重复 3 代；保留独占切片|Writing 下分配、释放并继续分配；WriteFinished 保留切片；交出写入端后释放；显式扫描；覆盖空块和多代复用|Writing/WriteFinished 不提前回收；LeaveWriter 且全释放后可回收；扫描才减少占用并每块通知一次；每代内容正确|
|M_complete_NN|M2 统一物理块完成入口|NN 策略；单块池；重复 3 代；保留独占切片|Writing 下分配、释放并继续分配；WriteFinished 保留切片；交出写入端后释放；显式扫描；覆盖空块和多代复用|Writing/WriteFinished 不提前回收；LeaveWriter 且全释放后可回收；扫描才减少占用并每块通知一次；每代内容正确|
|M_source_complete|M2 源块共用写入结束状态|单源块；写入一条消息后 finish；回调保留帧|写入结束后检查源状态与物理状态；读取尾部并移交帧；再检查物理状态；释放帧|写入完成时源与基类均为 WriteFinished；完成记录出队后为 LeaveWriter；用户帧阻止回收，释放后 all_released 为 true|
|D_complete_free|D1 违规操作|未借出的 Free 物理块|直接调用公开的 to_write_finished|精确完成状态诊断和专用退出86；不能从 Free 转 WriteFinished|
|W_layout_u8_prefix_p1|W6 可选前缀的端到端字节完整性|u8；prefix；p1；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u8_prefix_p3|W6 可选前缀的端到端字节完整性|u8；prefix；p3；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u8_prefix_buffered|W6 可选前缀的端到端字节完整性|u8；prefix；buffered；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_slots_u8_prefix|W7 按实际前缀预留切片|u8；prefix；min=0；单块1100次写入|每次单字节写入后扫描两次；保留全部帧；完成后再逐帧核对及释放|1100个切片不耗尽；数据保持不变；全部释放前不复用源块|
|W_layout_u8_raw_p1|W6 可选前缀的端到端字节完整性|u8；raw；p1；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u8_raw_p3|W6 可选前缀的端到端字节完整性|u8；raw；p3；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u8_raw_buffered|W6 可选前缀的端到端字节完整性|u8；raw；buffered；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_slots_u8_raw|W7 按实际前缀预留切片|u8；raw；min=0；单块1100次写入|每次单字节写入后扫描两次；保留全部帧；完成后再逐帧核对及释放|1100个切片不耗尽；数据保持不变；全部释放前不复用源块|
|W_limits_u8|W8 长度类型及溢出|u8；块恰好容纳负载及前缀|先写负数、空指针、超类型范围或加法溢出长度；再写最大测试负载|非法写入无副作用；u8/u16上限和u32/u64的65536字节消息正确|
|W_layout_u16_prefix_p1|W6 可选前缀的端到端字节完整性|u16；prefix；p1；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u16_prefix_p3|W6 可选前缀的端到端字节完整性|u16；prefix；p3；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u16_prefix_buffered|W6 可选前缀的端到端字节完整性|u16；prefix；buffered；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_slots_u16_prefix|W7 按实际前缀预留切片|u16；prefix；min=0；单块1100次写入|每次单字节写入后扫描两次；保留全部帧；完成后再逐帧核对及释放|1100个切片不耗尽；数据保持不变；全部释放前不复用源块|
|W_layout_u16_raw_p1|W6 可选前缀的端到端字节完整性|u16；raw；p1；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u16_raw_p3|W6 可选前缀的端到端字节完整性|u16；raw；p3；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u16_raw_buffered|W6 可选前缀的端到端字节完整性|u16；raw；buffered；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_slots_u16_raw|W7 按实际前缀预留切片|u16；raw；min=0；单块1100次写入|每次单字节写入后扫描两次；保留全部帧；完成后再逐帧核对及释放|1100个切片不耗尽；数据保持不变；全部释放前不复用源块|
|W_limits_u16|W8 长度类型及溢出|u16；块恰好容纳负载及前缀|先写负数、空指针、超类型范围或加法溢出长度；再写最大测试负载|非法写入无副作用；u8/u16上限和u32/u64的65536字节消息正确|
|W_layout_u32_prefix_p1|W6 可选前缀的端到端字节完整性|u32；prefix；p1；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u32_prefix_p3|W6 可选前缀的端到端字节完整性|u32；prefix；p3；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u32_prefix_buffered|W6 可选前缀的端到端字节完整性|u32；prefix；buffered；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_slots_u32_prefix|W7 按实际前缀预留切片|u32；prefix；min=0；单块1100次写入|每次单字节写入后扫描两次；保留全部帧；完成后再逐帧核对及释放|1100个切片不耗尽；数据保持不变；全部释放前不复用源块|
|W_layout_u32_raw_p1|W6 可选前缀的端到端字节完整性|u32；raw；p1；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u32_raw_p3|W6 可选前缀的端到端字节完整性|u32；raw；p3；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u32_raw_buffered|W6 可选前缀的端到端字节完整性|u32；raw；buffered；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_slots_u32_raw|W7 按实际前缀预留切片|u32；raw；min=0；单块1100次写入|每次单字节写入后扫描两次；保留全部帧；完成后再逐帧核对及释放|1100个切片不耗尽；数据保持不变；全部释放前不复用源块|
|W_limits_u32|W8 长度类型及溢出|u32；块恰好容纳负载及前缀|先写负数、空指针、超类型范围或加法溢出长度；再写最大测试负载|非法写入无副作用；u8/u16上限和u32/u64的65536字节消息正确|
|W_layout_u64_prefix_p1|W6 可选前缀的端到端字节完整性|u64；prefix；p1；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u64_prefix_p3|W6 可选前缀的端到端字节完整性|u64；prefix；p3；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u64_prefix_buffered|W6 可选前缀的端到端字节完整性|u64；prefix；buffered；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_slots_u64_prefix|W7 按实际前缀预留切片|u64；prefix；min=0；单块1100次写入|每次单字节写入后扫描两次；保留全部帧；完成后再逐帧核对及释放|1100个切片不耗尽；数据保持不变；全部释放前不复用源块|
|W_layout_u64_raw_p1|W6 可选前缀的端到端字节完整性|u64；raw；p1；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u64_raw_p3|W6 可选前缀的端到端字节完整性|u64；raw；p3；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_layout_u64_raw_buffered|W6 可选前缀的端到端字节完整性|u64；raw；buffered；32字节源块|混合参数及动态字节写入；核对发布位置；拒收重试；换块；完成并排空|与独立编码逐字节相同；无半消息、漏数据、重复数据；源池满足回收条件|
|W_slots_u64_raw|W7 按实际前缀预留切片|u64；raw；min=0；单块1100次写入|每次单字节写入后扫描两次；保留全部帧；完成后再逐帧核对及释放|1100个切片不耗尽；数据保持不变；全部释放前不复用源块|
|W_limits_u64|W8 长度类型及溢出|u64；块恰好容纳负载及前缀|先写负数、空指针、超类型范围或加法溢出长度；再写最大测试负载|非法写入无副作用；u8/u16上限和u32/u64的65536字节消息正确|
|W_layout_empty_config|W7/W8 空写入和布局校验|全部前缀宽度；无前缀单字节块|验证槽位公式、除数及加法边界；不匹配配置构造；无前缀空写入后写单字节|非法配置抛invalid_argument；无前缀空写入false；有效写入成功|
|W_layout_registration|R5 无基类异构注册|u8带前缀与u64无前缀共用Service|空指针、重复及错误管理器注册；两写入器各写40条后排空|精确错误码不变；线程编号和字节数据分别匹配|
|W_layout_worker|R5 无基类后台读取|u8带前缀与u64无前缀共用Worker|启动读取线程；两个类型写入；finish及stop后核对所有字节|异步搬运正确；统计准确；源块全归还|
|T_len_signed|T2 长度类型及无前缀静态约束|Writer<1,true,int> writer(nullptr,0);|合法对照编译成功后编译非法调用|TDataLen must be an unqualified unsigned integer of at most 64 bits|
|T_len_bool|T2 长度类型及无前缀静态约束|Writer<1,true,bool> writer(nullptr,0);|合法对照编译成功后编译非法调用|TDataLen must be an unqualified unsigned integer of at most 64 bits|
|T_len_float|T2 长度类型及无前缀静态约束|Writer<1,true,double> writer(nullptr,0);|合法对照编译成功后编译非法调用|TDataLen must be an unqualified unsigned integer of at most 64 bits|
|T_len_const|T2 长度类型及无前缀静态约束|Writer<1,true,const unsigned> writer(nullptr,0);|合法对照编译成功后编译非法调用|TDataLen must be an unqualified unsigned integer of at most 64 bits|
|T_len_u8_payload|T2 长度类型及无前缀静态约束|struct Large {char data[256];}; void f(WriterQueue* p){Writer<1,true,std::uint8_t> w(p,0);Large v{};(void)w.write(v);}|合法对照编译成功后编译非法调用|payload does not fit TDataLen|
|T_raw_empty|T2 长度类型及无前缀静态约束|void f(WriterQueue* p){Writer<1,false> w(p,0);(void)w.write();}|合法对照编译成功后编译非法调用|unframed payload must not be empty|
|W_pressure_timeout_invalid|W9 Writer 持有等待策略|Writer超时为0、负数、超出时钟范围|分别构造 Writer|均抛出invalid_argument；拒绝发生在任何等待之前|
|W_pressure_timeout_limit|W9 Writer 持有等待策略|Writer使用时钟允许的最大毫秒时限|构造 Writer 后发送并排空当前块|边界合法，不误拒绝|
|W_pressure_spin_timeout|W9 Writer 持有等待策略|单块写入器；最大自旋次数，30ms总时限|写满块，连续两次尝试写入并超时；读取后重试成功|自旋受到总期限约束；累计和最大阻塞计数正确；原数据及重试数据完整|
