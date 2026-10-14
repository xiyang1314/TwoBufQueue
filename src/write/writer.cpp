#include "TwoBufQueue/src/write/writer.hpp"

namespace TwoBufQueue {

// Compile the default per-record publication instance; other counts instantiate where used.
template class Writer<1, true>;   // With length prefixes (default uint16_t).
template class Writer<1, false>;  // No length prefix.

} // namespace TwoBufQueue
