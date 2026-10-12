#include "TwoBufQueue/src/write/seq_writer.hpp"

namespace TwoBufQueue {

// 编译默认逐条发布实例；其他发布次数由使用处按需实例化。
template class SeqWriter<std::uint32_t, 1, true>;   // 带长度前缀（默认 uint16_t）
template class SeqWriter<std::uint64_t, 1, true>;   // 64 位序号
template class SeqWriter<std::uint32_t, 1, false>;  // 无长度前缀

} // namespace TwoBufQueue
