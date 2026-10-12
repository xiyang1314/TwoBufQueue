#pragma once
#include "TwoBufQueue/src/data/spsc_write_block.hpp"
#include "TwoBufQueue/src/base_components/LiveHandoffQueue/spsc_live_handoff_queue.hpp"
#include "TwoBufQueue/src/write/types.hpp"

namespace TwoBufQueue {
using WriterPool = WarmPool<SpscWriteBlock, true, SpscBlockFactory<SpscWriteBlock>>;
using WriterQueue = SpscLiveHandoffQueue<WriterPool>;

// 只提供类型别名和块生命周期回调，不引入新的管理对象。
inline constexpr WriterQueue::Callbacks writer_queue_callbacks{
    [](SpscWriteBlock& block) noexcept { if (!block.to_write_finished()) std::terminate(); },
    [](const SpscWriteBlock& block) noexcept { return block.is_alloc_finish(); },
    [](SpscWriteBlock& block) noexcept {
        if (!block.to_leave_writer() || !block.finish()) std::terminate();
    }
};

[[nodiscard]] inline WriterQueue make_writer_queue(WriterPool& pool, std::size_t capacity = 256,
                                                  bool enable_stats = false) {
    return WriterQueue(pool, capacity, writer_queue_callbacks, enable_stats);
}

// make_writer 的源侧参数，全部可选，不传即用默认值。
// 只包含源侧（源块池 / 交接队列 / writer）的构造参数；读取侧输出参数见 TwoBufQueueOptions。
struct WriterSourceConfig {
    std::size_t block_size = 64 * 1024;      // 单个源块字节数
    std::size_t pool_min = 2;                // 源块池最小/初始数量
    std::size_t pool_max = 8;                // 源块池最大数量
    std::int32_t thread_id = 0;              // 写入线程标识，读取侧可据此区分来源
    std::size_t queue_capacity = 256;        // 源块交接队列容量
    WriterBackpressureConfig backpressure{}; // 背压策略，默认关闭
};

// 内部存储：持有源块池 + 交接队列，作为 OwnedWriter 的第一个基类，
// 先于 WriterT 构造、后于其析构，保证 writer 存活期间 pool/queue 始终有效。
struct WriterSourceStorage {
    WriterPool pool;
    WriterQueue queue;
    explicit WriterSourceStorage(const WriterSourceConfig& config)
        : pool(config.pool_min, config.pool_max, SpscBlockFactory<SpscWriteBlock>{config.block_size}),
          queue(make_writer_queue(pool, config.queue_capacity)) {}
};

// 自持有写入器：公开继承 WriterT，因此可直接作为 writer 传入 TwoBufQueue / ReadWorker；
// pool / queue 以私有基类隐藏，用户无需关心。WriterT 仅在基类初始化处要求完整类型，
// 故本头不依赖 writer.hpp / seq_writer.hpp。
template <class WriterT>
struct OwnedWriter : private WriterSourceStorage, public WriterT {
    explicit OwnedWriter(const WriterSourceConfig& config = {})
        : WriterSourceStorage(config),
          WriterT(&static_cast<WriterSourceStorage&>(*this).queue, config.thread_id,
                  config.backpressure) {}
};

// 一次性构造源块池 + 交接队列 + WriterT（普通 Writer 或 SeqWriter），返回可直接使用的 writer。
// 参数由 WriterSourceConfig 封装，全部可选。
// 用法：
//   auto writer = make_writer<Writer<>>();                     // 全部用默认值
//   auto writer = make_writer<SeqWriter<std::uint32_t>>();     // 带 seq 的写入器
//   auto writer = make_writer<Writer<>>({.block_size = 4096}); // 只覆盖需要的字段
//   TwoBufQueue queue(callback, writer);                      // 直接把 writer 传给上层
template <class WriterT>
[[nodiscard]] OwnedWriter<WriterT> make_writer(const WriterSourceConfig& config = {}) {
    return OwnedWriter<WriterT>(config);
}
} // namespace TwoBufQueue
