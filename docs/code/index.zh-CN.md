[跳到代码](#代码阅读)

[TwoBufQueue C++](../../README.zh-CN.md)

[首页](../../README.zh-CN.md) · [接口](../API.zh-CN.md) · [使用示例](../../examples/README.zh-CN.md) · [代码阅读](index.zh-CN.md)

[English](index.md) · [中文](index.zh-CN.md)

TWOBUFQUEUE · SOURCE BROWSER

# 代码阅读

先看基础示例，再按需要阅读批量、并发与同步回调。每个文件都提供完整代码、语法高亮和行号。

[基础示例 ↗](../../examples/01_basic.cpp) · [接口使用指南](../API.zh-CN.md)

[使用示例](#使用示例) · [组件实现](#组件实现) · [性能测试脚本](#性能测试脚本) · [汇编与指令记录](#汇编与指令记录) · [工程接入配置](#工程接入配置) · [技术记录](#技术记录)

## 使用示例

从基础收发到批量、并发和同步回调。

[01_basic.cpp](../../examples/01_basic.cpp)

最小完整流程：创建发送入口，接收消息，再结束发送并等待处理完。

examples/01_basic.cpp

C++ · 42 行

[02_batch_callback.cpp](../../examples/two_buf_queue/02_batch_callback.cpp)

用整批回调接收数据，并读取每个来源范围中的记录。

examples/two_buf_queue/02_batch_callback.cpp

C++ · 34 行

[03_multi_thread_writer.cpp](../../examples/two_buf_queue/03_multi_thread_writer.cpp)

多个生产线程各用自己的 Writer，后台接收并校验消息。

examples/two_buf_queue/03_multi_thread_writer.cpp

C++ · 63 行

[04_layer1_callback.cpp](../../examples/two_buf_queue/04_layer1_callback.cpp)

直接读取第一层源块，省去第二层复制。

examples/two_buf_queue/04_layer1_callback.cpp

C++ · 62 行

[05_seq_writer.cpp](../../examples/two_buf_queue/05_seq_writer.cpp)

给消息分配序号，并通过回调读取来源、序号和负载。

examples/two_buf_queue/05_seq_writer.cpp

C++ · 74 行

[06_sync_writer.cpp](../../examples/two_buf_queue/06_sync_writer.cpp)

在生产者线程执行接收回调；write 返回前完成消息处理。

examples/two_buf_queue/06_sync_writer.cpp

C++ · 49 行

[07_quick_write_muti_msg.cpp](../../examples/two_buf_queue/07_quick_write_muti_msg.cpp)

用双层循环快速写多条消息：内层写当前块，外层明确选择下一块。

examples/two_buf_queue/07_quick_write_muti_msg.cpp

C++ · 48 行

[08_all_parameters.cpp](../../examples/two_buf_queue/08_all_parameters.cpp)

逐项展示 Writer、两层内存池、读取与消费线程的全部参数。

examples/two_buf_queue/08_all_parameters.cpp

C++ · 111 行

[09_async_logger.cpp](../../examples/two_buf_queue/09_async_logger.cpp)

把业务日志交给后台写文件，演示缓存满时的处理与安全退出。

examples/two_buf_queue/09_async_logger.cpp

C++ · 114 行

[11_warm_pool.cpp](../../examples/components/11_warm_pool.cpp)

借出和归还池中资源，观察资源何时可再次使用。

examples/components/11_warm_pool.cpp

C++ · 48 行

[12_warm_pool_factory.cpp](../../examples/components/12_warm_pool_factory.cpp)

为内存池提供自定义资源创建和销毁方法。

examples/components/12_warm_pool_factory.cpp

C++ · 55 行

[13_spsc_live_handoff_queue.cpp](../../examples/components/13_spsc_live_handoff_queue.cpp)

读取尚在生产中的资源范围，并在处理完后归还。

examples/components/13_spsc_live_handoff_queue.cpp

C++ · 149 行

[14_spsc_live_handoff_queue_claim.cpp](../../examples/components/14_spsc_live_handoff_queue_claim.cpp)

领取队首资源；暂时不能处理时取消领取并保留记录。

examples/components/14_spsc_live_handoff_queue_claim.cpp

C++ · 155 行

[15_handoff_queue.cpp](../../examples/components/15_handoff_queue.cpp)

使用内置消费线程处理交接资源，拒绝时等待重试。

examples/components/15_handoff_queue.cpp

C++ · 96 行

[16_handoff_queue_mpmc.cpp](../../examples/components/16_handoff_queue_mpmc.cpp)

多个生产者和消费者共享交接队列，校验并发处理结果。

examples/components/16_handoff_queue_mpmc.cpp

C++ · 87 行

[example_support.hpp](../../examples/example_support.hpp)

示例共用的检查、解析与辅助函数。

examples/example_support.hpp

C++ · 54 行

## 组件实现

需要了解底层行为时，再读这些头文件。

[handoff_queue.hpp](../../src/base_components/HandoffQueue/handoff_queue.hpp)

资源交接队列实现，包含多种并发模式和消费工作线程。

src/base_components/HandoffQueue/handoff_queue.hpp

C++ · 102 行

[spsc_live_handoff_queue.hpp](../../src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp)

单生产者与单消费者之间交接资源，并支持读取正在生产的内容。

src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp

C++ · 353 行

[warm_pool.hpp](../../src/base_components/WarmPool/warm_pool.hpp)

资源池实现：创建、借出、归还和容量控制。

src/base_components/WarmPool/warm_pool.hpp

C++ · 375 行

## 性能测试脚本

测试执行、参数选择与历史结果呈现。

[plot_release.py](../../benchmarks/plot_release.py)

从保存的测量结果生成性能图表，不重新执行性能测试。

benchmarks/plot_release.py

Python · 70 行

[run_release_benchmarks.py](../../benchmarks/run_release_benchmarks.py)

按配置运行正式测试、校验负载，并保存环境和全部测量记录。

benchmarks/run_release_benchmarks.py

Python · 219 行

[run.py](../performance_data/latency32_investigation_20261010/run.py)

保存的性能实验脚本，保留当时的测试与结果处理逻辑。

docs/performance_data/latency32_investigation_20261010/run.py

Python · 43 行

[restyle_comparison.py](../performance_data/plateau_compare_256m_zero_20261012/restyle_comparison.py)

保存的性能实验脚本，保留当时的测试与结果处理逻辑。

docs/performance_data/plateau_compare_256m_zero_20261012/restyle_comparison.py

Python · 312 行

[select_weighted.py](../performance_data/plateau_compare_256m_zero_20261012/weighted_common_config_20261012/select_weighted.py)

保存的性能实验脚本，保留当时的测试与结果处理逻辑。

docs/performance_data/plateau_compare_256m_zero_20261012/weighted_common_config_20261012/select_weighted.py

Python · 185 行

## 汇编与指令记录

性能诊断中保存的实际指令。

[business_original.asm](../performance_data/burst_instruction_1m_20261012/business_original.asm)

性能分析中保存的汇编指令，可按行定位并阅读。

docs/performance_data/burst_instruction_1m_20261012/business_original.asm

Assembly · 45 行

[push_codegen.s](../performance_data/rigtorp_prefetch_20261011/push_codegen.s)

性能分析中保存的汇编指令，可按行定位并阅读。

docs/performance_data/rigtorp_prefetch_20261011/push_codegen.s

Assembly · 66 行

## 工程接入配置

把组件接入应用工程的 CMake 配置。

[CMakeLists.txt](../../test/integration/CMakeLists.txt)

验证本地源码、FetchContent 和安装包三种工程接入方式。

test/integration/CMakeLists.txt

CMake · 32 行

## 技术记录

保存的诊断说明与实验记录。

[README.txt](../performance_data/twobufqueue_8b_regression_20261011/README.txt)

保存的技术说明与实验记录。

docs/performance_data/twobufqueue_8b_regression_20261011/README.txt

Text · 35 行

TwoBufQueue · C++ · Apache-2.0 [首页](../../README.zh-CN.md)
