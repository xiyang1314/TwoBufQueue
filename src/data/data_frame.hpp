#pragma once
#include <cstddef>
#include <cstdint>
namespace TwoBufQueue {
// Input view retained synchronously; keep data readable until the call returns. Does not own source memory.
struct DataView {
    std::int32_t thread_id = -1;
    const std::byte* data = nullptr;
    std::size_t size = 0;
};

// One source-range view within a ReadBlock, possibly containing multiple records; owns no Slice.
// thread_id is the Writer's registered source id, not an operating-system thread id.
struct DataFrame {
    std::int32_t thread_id = -1;
    const std::byte* data = nullptr;
    std::size_t size = 0;
    std::int64_t read_time_ns = 0;
};

}
