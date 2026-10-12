#pragma once

#include "TwoBufQueue/src/data/spsc_write_block.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace TwoBufQueue {

// 内存读取模块的错误类别；回调暂未处理全部数据属于背压，不属于错误。
enum class ReadError : std::uint32_t {
    none,  // 无错误
    null_dependency,  // 写入器、管理器或读取处理回调为空
    invalid_registration,  // 注册参数无效或正在运行
    duplicate_writer,  // 写入器、管理器或线程编号重复
    foreign_current_range,  // 当前块不属于指定管理器
    registration_allocation_failed,  // 注册时创建保存资源失败
    invalid_start_state,  // 启动配置或状态无效
    thread_creation_failed,  // 创建读取线程失败
    cpu_affinity_failed,  // 设置读取线程 CPU 亲和性失败
    foreign_queued_range,  // 完成队列中出现其他管理器的块
    range_release_failed,  // 归还已保存块失败
    free_range,  // 保存的块为空或已归还
    invalid_range_cursors,  // 块的读写位置无效
    collection_capacity_exceeded,  // 本轮读取收集数组容量不足
    invalid_read_callback_result,  // 内部收集确认数量不一致
    finish_timeout,  // 慢退超过总时限，源端仍可能有未处理数据
};

// 一轮的结果。pending 包括背压、尚未到期的 current，以及下一轮的记录。
// 生产线程停止后，force=true 且 pending=false 才表示本轮已收齐源数据。
// is_scan_current=false 时仅覆盖已交接块；尾部须由 Writer.finish() 交接后再排空。
struct ReadResult {
    bool progressed = false;
    bool pending = false;
    ReadError error = ReadError::none;
};

// 调用方选择读取线程的退出方式；外部存储负责已接收数据的发布和生命周期。
enum class ReadStopMode : std::uint8_t {
    slow,  // 慢退：收齐源块的剩余数据并交给回调。
    fast,  // 快退：当前回调返回后停止继续收集。
};

// 读取线程的启动参数；对象构造后复制保存，运行期间不支持动态修改。
struct ReadConfig {
    // 没有完成块时执行 pause 轮询的次数，零表示直接休眠。
    std::uint32_t idle_spin_count = 1'000;
    // 初始扫描间隔；连续两次空扫描时翻倍，扫描到数据时恢复此间隔，与回调结果无关。
    std::chrono::nanoseconds idle_rescan_interval = std::chrono::milliseconds{5};
    // 读取线程绑定的逻辑 CPU；负数表示交给操作系统调度。
    std::int32_t worker_cpu = -1;
    // 连续空扫描的等待间隔上限，必须不小于初始间隔；达到上限后保持不变。
    std::chrono::nanoseconds idle_rescan_max_interval = std::chrono::milliseconds{30};
    // 慢退收尾的总时限；必须为正，通知和局部进展不会重置此时限。
    // 超时记录 finish_timeout 后退出，未处理的源数据保留原处。
    std::chrono::milliseconds finish_timeout = std::chrono::seconds{5};
    // 普通等待的最小时长；与最近扫描剩余时长取较大值，事件可提前唤醒。
    std::chrono::milliseconds work_wait_min_interval = std::chrono::milliseconds{1};
    // 收尾未完成时的重试间隔；实际等待还受 finish_timeout 剩余时长限制。
    std::chrono::milliseconds finish_retry_interval = std::chrono::milliseconds{1};
    // false：仅读取完成块，flush 通知也不读取 current；尾部须交接当前块后交付。
    // 配合 Writer 的 uint32_t 最大发布次数，可省去逐条发布与发布计数。
    bool is_scan_current = true;
};

// 读取线程的累计统计快照；不包含外部存储的批次/块池统计。
struct ReadStats {
    // 回调逐块确认的非空源块增量次数。
    std::uint64_t saved_frames = 0;
    // 回调确认接收的原始数据总字节数；该块成功后立即推进读取位置。
    std::uint64_t saved_data_bytes = 0;
    // 完成记录已出队并归还的源块数量。
    std::uint64_t saved_finished_ranges = 0;
    // 通过空闲扫描保存的活动块增量帧数量。
    std::uint64_t saved_partial_frames = 0;
    // 保留统计字段；新流程不等待写入停顿，恒为零。
    std::uint64_t scan_growth_count = 0;
    // 保留统计字段；新流程不保存跨轮观察快照，恒为零。
    std::uint64_t sequence_reset_count = 0;
    // Service 汇总逐块回调返回 false 的累计次数。
    std::uint64_t memory_full_events = 0;
    // Service 按写入线程记录首个未处理项，并按“块地址+借出代号+读取位置”去重。
    std::uint64_t memory_backpressured_frames = 0;
    // 生命周期内检测到的配置、队列、保存及回调错误总数。
    std::uint64_t error_count = 0;
};

}  // namespace TwoBufQueue
