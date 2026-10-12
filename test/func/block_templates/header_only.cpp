#include "TwoBufQueue/src/base_components/block_slice/writer_block.hpp"

template struct TwoBufQueue::WriterBlock<false, false>;
template struct TwoBufQueue::WriterBlock<false, true>;
template struct TwoBufQueue::WriterBlock<true, false>;
template struct TwoBufQueue::WriterBlock<true, true>;

int main() {
    TwoBufQueue::WriterBlock<false, false> same_thread;
    TwoBufQueue::WriterBlock<true, true> concurrent;
    return same_thread.is_free() && concurrent.is_free() ? 0 : 1;
}
