#include "TwoBufQueue/src/write/seq_writer.hpp"

namespace TwoBufQueue {

// Compile the default per-record publication instance; other counts instantiate where used.
template class SeqWriter<std::uint32_t, 1, true>;   // With length prefixes (default uint16_t).
template class SeqWriter<std::uint64_t, 1, true>;   // 64-bit sequence numbers.
template class SeqWriter<std::uint32_t, 1, false>;  // No length prefix.

} // namespace TwoBufQueue
