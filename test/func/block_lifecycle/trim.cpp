#include "../../../support/joining_thread.hpp"
#include "../../warm_pool_test_support.hpp"
#include "TwoBufQueue/src/base_components/block_slice/slice_pool.hpp"
#include <array>
#include <barrier>
#include <latch>
#include <iostream>
#include <thread>
#include <string_view>
using namespace TwoBufQueue;
#define CHECK(x) do { if (!(x)) { std::cerr << #x << ':' << __LINE__ << '\n'; std::abort(); } } while (false)
template<class B> void limits() {
    for (std::size_t minimum : {0u, 2u}) for (std::size_t cache : {0u, 1u, 4u}) {
        auto pool = make_block_pool<TestBlockPool<B>>(64, minimum, 4, {}, cache);
        for (int round = 0; round != 10; ++round) {
            std::array<B*,4> blocks;
            for (auto& block : blocks) { block = pool.get().get(); CHECK(block); }
            CHECK(!pool.get().get());
            for (auto* block : blocks) block->to_alloc_finished();
            CHECK(pool.all_released());
            CHECK(pool.resource_count() == std::max(minimum, cache));
            CHECK(pool.free_resource_count() == pool.resource_count());
        }
    }
}
void held() {
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(64, 0, 2, {}, 0);
    auto* block = pool.get().get(); auto slice = block->allocate(16);
    slice.data()[0] = std::byte{42}; block->to_alloc_finished();
    CHECK(pool.resource_count() == 1 && pool.in_use_count() == 1);
    CHECK(slice.data()[0] == std::byte{42}); slice.release();
    CHECK(pool.resource_count() == 0 && pool.all_released());
    // 最后一个切片退出受保护的释放操作后，Block 对象也可以销毁。
}
void preallocate() {
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(64, 1, 4, {}, 1);
    CHECK(pool.preallocate(4) && pool.free_resource_count() == 4);
    for (int i=0; i<3; ++i) pool.get().get()->to_alloc_finished();
    CHECK(pool.resource_count() == 1 && !pool.preallocate(5));
}
void hot_prefix() {
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(64, 0, 3, {}, 2);
    auto* a=pool.get().get(); auto* b=pool.get().get(); auto c_loan=pool.get(); auto* c=c_loan.get();
    a->to_alloc_finished(); b->to_alloc_finished(); c->to_alloc_finished();
    CHECK(!pool.is_valid(c_loan));
    auto* hot=pool.get().get(); auto* next=pool.get().get(); CHECK(hot==b && next==a);
    hot->to_alloc_finished(); next->to_alloc_finished();
}
struct FailingBlock : ReaderBlock<> {
    using ReaderBlock<>::ReaderBlock;
    inline static bool fail = false;
    void prepare(std::size_t capacity) {
        ReaderBlock<>::prepare(capacity);
        if (fail) throw std::bad_alloc{};
    }
};
void restore_failure() {
    auto pool = make_block_pool<TestBlockPool<FailingBlock>>(64, 0, 1, {}, 0);
    auto first = pool.get(); CHECK(first); first->to_alloc_finished();
    FailingBlock::fail = true;
    CHECK(!pool.get().get() && pool.resource_count() == 0 && pool.all_released());
    CHECK(!pool.is_valid(first));
    FailingBlock::fail = false;
    auto* next = pool.get().get(); CHECK(next && next->capacity() == 64);
    next->to_alloc_finished();
}
void concurrent() {
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(64, 0, 1, {}, 0);
    std::atomic<unsigned> callbacks{0};
    pool.set_release_callback([](void* p) noexcept { ++*static_cast<std::atomic<unsigned>*>(p); }, &callbacks);
    for (unsigned i=0; i<1000; ++i) {
        auto* block=pool.get().get(); CHECK(block);
        auto a=block->allocate(16); auto b=block->allocate(16); std::barrier start(3);
        brq_support::JoiningThread first([&] { start.arrive_and_wait(); a.release(); });
        brq_support::JoiningThread second([&] { start.arrive_and_wait(); b.release(); });
        start.arrive_and_wait(); block->to_alloc_finished(); first.join(); second.join();
        CHECK(pool.all_released() && pool.resource_count()==0 && callbacks==i+1);
    }
}
void stale(bool death, bool reuse) {
    if (death) BlockDiagnostics<ReaderBlock<>::StorageBlock>::set_error_handler([](const auto&) noexcept { std::_Exit(91); });
    auto pool = make_block_pool<TestBlockPool<ReaderBlock<>>>(64, 0, 1, {.is_check_slice_release=death}, 0);
    auto* block=pool.get().get(); auto old=block->allocate(16);
    block->to_alloc_finished(); block->force_recycle();
    CHECK(pool.all_released() && pool.resource_count()==0);
    auto* next=reuse ? pool.get().get() : nullptr;
    old.release();
    if (next) { CHECK(pool.is_valid(*next)); next->to_alloc_finished(); }
}

struct CountedBlock : ReaderBlock<> {
    using ReaderBlock<>::ReaderBlock;
    inline static std::atomic<unsigned> destroys{0};
    ~CountedBlock() noexcept { ++destroys; }
};
void actual_destroy() {
    CountedBlock::destroys = 0;
    {
        auto pool = make_block_pool<TestBlockPool<CountedBlock>>(64, 0, 1, {.is_check_slice_release=false}, 0);
        auto first = pool.get();
        auto old = first->allocate(16);
        first->to_alloc_finished(); first->force_recycle();
        CHECK(CountedBlock::destroys == 0 && !pool.is_valid(first));
        auto moved = std::move(old);
        auto next = pool.get(); CHECK(next);
        next->to_alloc_finished(); // 本次登记触发旧对象的真正销毁。
        CHECK(CountedBlock::destroys == 1);
        auto fresh_loan = pool.get(); CHECK(fresh_loan);
        auto fresh = fresh_loan->allocate(16);
        fresh.data()[0] = std::byte{91};
        moved.release(); // 已 delete 的 Block，必须只检查保存的凭证。
        CHECK(fresh_loan->outstanding_count() == 1 && fresh.data()[0] == std::byte{91});
        fresh_loan->to_alloc_finished(); fresh.release();
        CHECK(CountedBlock::destroys == 2 && pool.all_released());
    }
    CHECK(CountedBlock::destroys == 3);
}
void slice_access_overlap() {
    CountedBlock::destroys = 0;
    auto pool = make_block_pool<TestBlockPool<CountedBlock>>(64, 0, 2, {.is_check_slice_release=false}, 0);
    auto loan = pool.get(); auto a = loan->allocate(16); auto b = loan->allocate(16);
    loan->to_alloc_finished();
    std::latch entered(1), resume(1);
    std::thread worker([&] {
        {
            std::lock_guard guard(loan);
            CHECK(loan.is_valid());
            entered.count_down(); resume.wait();
            CHECK(loan->capacity() == 64 && CountedBlock::destroys == 0);
        }
        a.release();
    });
    entered.wait();
    // force_recycle 与 Slice 共用 Node 锁；手工持锁时不能递归调用它。
    std::thread force([&] { loan->force_recycle(); });
    auto unrelated = pool.get(); CHECK(unrelated); // Node 锁不阻塞其他节点。
    unrelated->to_alloc_finished();
    CHECK(CountedBlock::destroys == 0);
    resume.count_down(); worker.join(); force.join();
    b.release();
    CHECK(!pool.is_valid(loan) && pool.all_released() && CountedBlock::destroys == 1);
}

int main(int argc,char** argv) {
    if(argc!=2) return 2;
    std::string_view c=argv[1];
    if(c=="single") limits<ReaderBlock<false,false>>();
    else if(c=="1n") limits<ReaderBlock<false,true>>();
    else if(c=="nn") limits<ReaderBlock<true,true>>();
    else if(c=="held") held();
    else if(c=="actual_destroy") actual_destroy();
    else if(c=="slice_access_overlap") slice_access_overlap();
    else if(c=="preallocate") preallocate();
    else if(c=="hot_prefix") hot_prefix();
    else if(c=="restore_failure") restore_failure();
    else if(c=="concurrent") concurrent();
    else if(c=="force_ignore") { stale(false,false); stale(false,true); }
    else if(c=="death_free") stale(true,false);
    else if(c=="death_stale") stale(true,true);
    else return 2;
}
