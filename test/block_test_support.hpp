#pragma once

namespace TwoBufQueue {
// 按块的角色结束分配；WriterBlock<> 还需要完成写入端交接。
template<class Block> void finish_block(Block* block) {
    if constexpr (requires { block->to_alloc_finished(); }) block->to_alloc_finished();
    else { block->to_write_finished(); block->to_leave_writer(); }
}
} // namespace TwoBufQueue
