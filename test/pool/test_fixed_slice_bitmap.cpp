#include "../support/warm_pool_test_support.hpp"
#include "../support/block_test_support.hpp"
#include "TwoBufQueue/src/base_components/block_slice/block_resource_factory.hpp"
#include "TwoBufQueue/src/base_components/block_slice/reader_block.hpp"
#include "../func/support.hpp"
#include <cassert>
#include <thread>
#include <vector>
#include <iostream>

using namespace TwoBufQueue;
using Pool = TestBlockPool<ReaderBlock<>>;

static void boundaries() {
    for (std::size_t cap : {1u, 63u, 64u, 65u, 127u, 128u, 129u}) {
        auto pool = make_block_pool<Pool>(4096, 1, 1,
                  ReaderBlock<>::Config{.max_slices_per_block = cap});
        for (unsigned pass = 0; pass < 3; ++pass) {
            auto* block = pool.get().get();
            std::vector<MemorySlice> slices;
            for (std::size_t i = 0; i < cap; ++i) {
                auto slice = block->allocate(1);
                assert(slice);
                *slice.data() = static_cast<std::byte>(i);
                slices.push_back(std::move(slice));
            }
            assert(!block->allocate(1)); // Bytes remain, but all slots are used.
            assert(block->outstanding_count() == cap);
            slices[0].release();
            assert(!block->allocate(1)); // Released slots are not reused this generation.
            for (std::size_t i = cap; i-- > 1;) {
                assert(*slices[i].data() == static_cast<std::byte>(i));
                slices[i].release();
            }
            finish_block(block);
            assert(pool.in_use_count() == 0);
        }
    }
    bool rejected = false;
    try {
        auto pool = make_block_pool<Pool>(64, 0, 1,
                  ReaderBlock<>::Config{.max_slices_per_block = 0});
    } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected); // Reject even with no initial blocks.
}

static void concurrent_bits() {
    auto pool = make_block_pool<Pool>(4096, 1, 1,
              ReaderBlock<>::Config{.max_slices_per_block = 129});
    for (unsigned pass = 0; pass < 20; ++pass) {
        auto* block = pool.get().get();
        std::vector<MemorySlice> slices;
        for (int i = 0; i < 129; ++i) slices.push_back(block->allocate(1));
        finish_block(block);
        std::vector<std::thread> threads;
        for (std::size_t t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (std::size_t i = t; i < slices.size(); i += 4) slices[i].release();
            });
        }
        for (auto& thread : threads) thread.join();
        assert(pool.in_use_count() == 0);
    }
}

static void storage_rollover() {
    ft::ReadBlockPool pool(1, 1, ReadBlockFactory{64, 2});
    ft::OutputQueue queue(pool, 1);
    const std::byte bytes[]{std::byte{1}, std::byte{2}, std::byte{3}};
    auto acquired = queue.try_acquire();
    assert(acquired);
    auto loan = acquired.loan;
    assert(loan->append(0, bytes, 1, 42) && loan->append(0, bytes + 1, 1, 42));
    assert(!loan->append(0, bytes + 2, 1, 42)); // Frame slots exhausted, bytes remain.
    assert(queue.send(loan));
    auto received = queue.try_receive();
    assert(received && received->data_size() == 2 && received->frames().size() == 2);
    assert(*received->frames()[0].data == bytes[0] && *received->frames()[1].data == bytes[1]);
    assert(!queue.try_acquire()); // Receiving does not release the output block.
    assert(queue.finish(received));
    auto next = queue.try_acquire(); assert(next);
    assert(next.loan->frames().empty() && next.loan->append(0, bytes + 2, 1, 43));
    assert(queue.send(next.loan)); queue.close();
    received = queue.try_receive();
    assert(received && *received->frames()[0].data == bytes[2]);
    assert(queue.finish(received) && pool.all_released());
}

int main() {
    boundaries(); concurrent_bits(); storage_rollover();
    std::cout << "bitmap boundaries, reuse, concurrent release and slot-full backpressure passed\n";
}
