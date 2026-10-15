#pragma once

namespace TwoBufQueue {
// End allocation according to block role; WriterBlock<> also needs writer-side handoff completion.
template<class TBlock> void finish_block(TBlock* block) {
    if constexpr (requires { block->to_alloc_finished(); }) block->to_alloc_finished();
    else { block->to_write_finished(); block->to_leave_writer(); }
}
} // namespace TwoBufQueue
