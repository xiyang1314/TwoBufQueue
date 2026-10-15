#include "../support/warm_pool_test_support.hpp"
#include "../support/block_test_support.hpp"
#include "TwoBufQueue/src/utils/no_lock.hpp"
#include "TwoBufQueue/src/utils/spin_lock.hpp"
#include <type_traits>
#include "TwoBufQueue/src/base_components/block_slice/slice_pool.hpp"
#include "TwoBufQueue/src/base_components/block_slice/block_resource_factory.hpp"
#include "TwoBufQueue/src/base_components/block_slice/reader_block.hpp"
#include <cassert>
#include <barrier>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <tuple>
#include <vector>

namespace TwoBufQueue {
class BlockSlicesAllocatorForTest {
public:
    template<class TBlock>
    static void release(TBlock* block, std::size_t index, std::uint64_t sequence) {
        block->slices_.release(index, sequence, std::source_location::current());
    }
    template<class TBlock>
    static auto snapshot(TBlock* block) {
        auto& m = block->slices_;
        std::vector<std::uint64_t> flags;
        const auto allocated = m.allocated_count_.load();
        const auto words = (allocated >> 6) + ((allocated & 63) != 0);
        for (std::size_t i = 0; i < words; ++i) flags.push_back(m.released_[i]);
        return std::tuple(allocated, m.released_count_, block->allocated_size(),
                          m.sequence_, block->state(), flags);
    }
};
}
using namespace TwoBufQueue;
using Pool = TestBlockPool<ReaderBlock<>>;
using Access = BlockSlicesAllocatorForTest;
static const char* expected_error;

template<class TBlock>
static void scenario(std::string_view kind, bool strict) {
    using Pool = TestBlockPool<TBlock, false>;
    ReaderBlock<>::Config config; // Default strict mode must remain enabled.
    assert(config.is_check_slice_release);
    if (!strict) config.is_check_slice_release = false;
    auto pool = make_block_pool<Pool>(64, 1, 1, config);
    auto* block = pool.get().get();
    auto first = block->allocate(8);
    auto second = block->allocate(8);
    std::uint64_t sequence = 1;
    std::size_t index = 0;
    if (kind == "generation") {
        first.release(); second.release(); finish_block(block);
        block = pool.get().get();
        first = block->allocate(8); second = block->allocate(8);
        expected_error = "slice belongs to an old block generation";
    } else if (kind == "zero") {
        sequence = 0;
        expected_error = "slice belongs to an old block generation";
    } else if (kind == "free") {
        first.release(); second.release(); finish_block(block);
        expected_error = "block is already released";
    } else if (kind == "index") {
        index = 999;
        expected_error = "slice index is out of range";
    } else if (kind == "duplicate") {
        first.release();
        auto extra = block->allocate(8); // Preserve earlier release bits in the same word.
        extra.release();
        expected_error = "slice is already released";
    } else std::abort();
    if (kind == "free") assert(pool.all_released());
    if (strict) {
        TestDiagnostics<Pool>::set_error_handler([](const typename TestDiagnostics<Pool>::ErrorInfo& info) noexcept {
            std::_Exit(std::strcmp(info.reason, expected_error) == 0 ? 77 : 78);
        });
    }
    const auto before = Access::snapshot(block);
    const auto count = pool.in_use_count();
    Access::release(block, index, sequence);
    assert(!strict);
    assert(before == Access::snapshot(block));
    assert(pool.in_use_count() == count);
    if (kind != "free") {
        finish_block(block);
        if (first) first.release();
        if (second) second.release();
    }
    assert(pool.in_use_count() == 0);
    // The block remains reusable and valid RAII release still recycles it.
    block = pool.get().get();
    { auto slice = block->allocate(8); finish_block(block); }
    assert(pool.in_use_count() == 0);
}
template<class TBlock>
static void seal_release_race(bool check) {
    using Pool = TestBlockPool<TBlock>;
    auto pool = make_block_pool<Pool>(64, 1, 1,
              ReaderBlock<>::Config{.is_check_slice_release = check});
    std::atomic<unsigned> recycled{0};
    pool.set_release_callback([](void* p) noexcept {
        static_cast<std::atomic<unsigned>*>(p)->fetch_add(1);
    }, &recycled);
    for (unsigned i = 0; i < 200; ++i) {
        auto* block = pool.get().get();
        auto slice = block->allocate(8);
        std::barrier start(2);
        std::thread consumer([held = std::move(slice), &start]() mutable {
            start.arrive_and_wait();
            held.release();
        });
        start.arrive_and_wait();
        finish_block(block);
        consumer.join();
        assert(pool.in_use_count() == 0);
        assert(recycled.load() == i + 1);
    }
    // An empty handed-off block needs no placeholder slice; scanning reclaims it.
    finish_block(pool.get().get());
    assert(pool.all_released());
    assert(recycled.load() == 201);
}
int main(int argc, char** argv) {
    if (argc >= 2) {
        const std::string_view mode = argc == 3 ? argv[2] : "Alloc1ReleaseN";
        if (mode == "Alloc1Release1") scenario<Alloc1Release1ReaderBlock>(argv[1], true);
        else if (mode == "AllocNReleaseN") scenario<AllocNReleaseNReaderBlock>(argv[1], true);
        else scenario<ReaderBlock<>>(argv[1], true);
        return 1;
    }
    for (auto kind : {"generation", "zero", "free", "index", "duplicate"}) {
        scenario<Alloc1Release1ReaderBlock>(kind, false);
        scenario<ReaderBlock<>>(kind, false);
        scenario<AllocNReleaseNReaderBlock>(kind, false);
    }
    seal_release_race<ReaderBlock<>>(true);
    seal_release_race<ReaderBlock<>>(false);
    seal_release_race<AllocNReleaseNReaderBlock>(true);
    seal_release_race<AllocNReleaseNReaderBlock>(false);
    std::cout << "all modes: release checks preserve metadata and recycling\n";
}
