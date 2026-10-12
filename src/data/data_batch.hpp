#pragma once
#include "TwoBufQueue/src/data/data_frame.hpp"
#include <span>
namespace TwoBufQueue {
// 整块的临时只读视图；复制/移动不延长 ReadBlock 的寿命，不承担资源所有权。
// 自动消费回调返回后不得保存 frame 或数据指针。每个 frame 自带读取时间，
// 允许同一块包含 completed/current 两个阶段的数据。
struct DataBatch {
    std::span<const DataFrame> frames;
    std::size_t data_size = 0;
};
}
