#pragma once

namespace TwoBufQueue {
// End allocation according to block role; WriterBlock<> also needs writer-side handoff completion.
template<class Block> void finish_block(Block* block) {
    if constexpr (requires { block->to_alloc_finished(); }) block->to_alloc_finished();
    else { block->to_write_finished(); block->to_leave_writer(); }
}
} // namespace TwoBufQueue
