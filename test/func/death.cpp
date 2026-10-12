#include "../writer_fixture.hpp"
#include "../warm_pool_test_support.hpp"
#include "support.hpp"
#include <cstdlib>
namespace ft {
static const char *expected_reason = nullptr;
void death_cases(Cases &c) {
    c.push_back({"D_complete_free", [] {
        using P = TestBlockPool<ReaderBlock<>>;
        TestDiagnostics<P>::set_error_handler([](const TestDiagnostics<P>::ErrorInfo &info) noexcept {
            if (std::strcmp(info.reason, "expected Allocating block") == 0) {
                std::fputs("EXPECTED_CONTRACT_FAILURE\n", stderr);
                std::fflush(stderr);
                std::_Exit(86);
            }
            std::fprintf(stderr, "WRONG_REASON: %s\n", info.reason);
            std::_Exit(87);
        });
        ReaderBlock<> unused({});
        unused.to_alloc_finished();
        throw std::runtime_error("expected contract failure did not occur");
    }});
    for (std::string k :
         {"empty", "double", "zero", "align3", "align128", "seal", "range", "foreign", "livepool"})
        c.push_back(
            {"D_" + k, [k] {
                 using P = TestBlockPool<ReaderBlock<>>;
                 TestDiagnostics<P>::set_error_handler([](const TestDiagnostics<P>::ErrorInfo &info) noexcept {
                     if (expected_reason && std::strcmp(info.reason, expected_reason) == 0) {
                         std::fputs("EXPECTED_CONTRACT_FAILURE\n", stderr);
                         std::fflush(stderr);
                         std::_Exit(86);
                     }
                     std::fprintf(stderr, "WRONG_REASON: %s\n", info.reason);
                     std::_Exit(87);
                 });
                 if (k == "empty" || k == "double") {
                     expected_reason = "cannot release an empty or already released slice";
                     MemorySlice empty;
                     if (k == "double") {
                         Alloc1ReleaseNSlicePool p(64, 1, 1);
                         auto s = p.allocate(1);
                         p.close();
                         s.release();
                         s.release();
                     } else
                         empty.release();
                 } else if (k == "zero" || k == "align3" || k == "align128") {
                     expected_reason = "slice size/alignment is invalid";
                     Alloc1ReleaseNSlicePool p(64, 1, 1);
                     (void)p.allocate(k == "zero" ? 0 : 1, k == "align3"     ? 3
                                                           : k == "align128" ? 128
                                                                             : 1);
                 } else if (k == "livepool") {
                     expected_reason = "destroying slice pool while blocks are still in use";
                     MemorySlice held;
                     {
                         Alloc1ReleaseNSlicePool p(64, 1, 1);
                         held = p.allocate(1);
                     }
                 } else {
                     auto p = make_block_pool<P>(64, 1, 1);
                     auto *b = p.get().get();
                     auto held = b->allocate(1);
                     if (k == "seal") {
                         expected_reason = "expected Allocating block";
                         b->to_alloc_finished();
                         b->to_alloc_finished();
                     }
                     if (k == "range") {
                         expected_reason = "source cursors must satisfy read <= published <= capacity";
                         auto source = make_block_pool<TestBlockPool<WriterBlock<>>>(64, 1, 1);
                         auto* w = source.get().get();
                         w->write_ptr_.store(reinterpret_cast<std::byte*>(reinterpret_cast<std::uintptr_t>(w->buf_) + 65));
                         (void)w->allocate_read_slice();
                     }
                     if (k == "foreign") {
                         expected_reason = "finished block must equal current producer";
                         std::set_terminate([] { std::fputs("EXPECTED_CONTRACT_FAILURE\n", stderr); std::_Exit(86); });
                         WriterFixture source(64, 1, 1);
                         Writer<> writer(&source.queue, 0, source.backpressure);
                         // 外部错误交接当前生产块，制造块与凭证错位；写满换块时被 acquire_next 拒绝。
                         CHECK(source.queue.send());
                         while (writer.write(11u)) {}
                     }
                 }
                 throw std::runtime_error("expected contract failure did not occur");
             }});
}
} // namespace ft
