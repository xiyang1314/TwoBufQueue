#pragma once
#include "TwoBufQueue/src/data/data_frame.hpp"
#include <span>
namespace TwoBufQueue {
// Temporary read-only whole-block view; copying/moving extends no ReadBlock lifetime and transfers no ownership.
// Never retain frames or data pointers after the automatic consumer callback returns. Each frame carries its read timestamp,
// allowing one block to contain data from both completed and current phases.
struct DataBatch {
    std::span<const DataFrame> frames;
    std::size_t data_size = 0;
};
}
