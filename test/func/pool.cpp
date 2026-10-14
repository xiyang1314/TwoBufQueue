#include "support.hpp"
namespace ft {
template <class Pool> void pool_mode(Cases &c, std::string mode) {
    for (std::size_t a : {1u, 2u, 4u, 8u, 16u, 32u, 64u})
        c.push_back({"P_align_" + mode + "_" + std::to_string(a), [a] {
                         Pool p(128, 1, 1);
                         auto first = p.allocate(1);
                         auto second = p.allocate(7, a);
                         CHECK(first && second);
                         CHECK(reinterpret_cast<std::uintptr_t>(second.data()) % a == 0);
                         CHECK(second.data() >= first.data() + 1);
                         std::memset(second.data(), 0x6a, 7);
                         CHECK(!p.allocate(129));
                         auto third = p.allocate(1);
                         CHECK(third);
                         p.close();
                         CHECK(!p.allocate(1));
                         CHECK(p.in_use_count() == 1);
                         for (std::size_t i = 0; i < 7; ++i)
                             CHECK(second.data()[i] == std::byte{0x6a});
                         third.release();
                         second.release();
                         first.release();
                         CHECK(p.all_released());
                     }});
    for (std::size_t n : {1u, 63u, 64u, 65u, 129u})
        c.push_back({"P_slots_" + mode + "_" + std::to_string(n), [n] {
                         Pool p(512, 0, 1, {.max_slices_per_block = n});
                         for (unsigned generation = 0; generation < 4; ++generation) {
                             std::vector<typename Pool::Slice> held;
                             for (std::size_t i = 0; i < n; ++i) {
                                 auto s = p.allocate(1);
                                 CHECK(s);
                                 *s.data() = std::byte(i & 255);
                                 held.push_back(std::move(s));
                             }
                             CHECK(!p.allocate(1));
                             CHECK(p.block_count() == 1);
                             for (std::size_t i = n; i > 0; --i) {
                                 CHECK(*held[i - 1].data() == std::byte((i - 1) & 255));
                                 held[i - 1].release();
                             }
                             CHECK(p.all_released());
                         }
                         p.close();
                     }});
    c.push_back({"P_move_" + mode, [] {
                     Pool p(64, 1, 1);
                     auto a = p.allocate(8);
                     auto b = p.allocate(8);
                     std::memset(a.data(), 77, 8);
                     auto *address = a.data();
                     p.close();
                     typename Pool::Slice moved(std::move(a));
                     CHECK(!a && a.data() == nullptr && a.size() == 0);
                     b = std::move(moved);
                     CHECK(!moved);
                     CHECK(b.data() == address);
                     auto *self = &b;
                     b = std::move(*self);
                     CHECK(b.data() == address);
                     CHECK(*b.data() == std::byte{77});
                     b.release();
                     CHECK(!b);
                     CHECK(p.all_released());
                 }});
}
void pool_cases(Cases &c) {
    pool_mode<Alloc1Release1SlicePool>(c, "11");
    pool_mode<Alloc1ReleaseNSlicePool>(c, "1N");
    pool_mode<AllocNReleaseNSlicePool>(c, "NN");
    for (std::string k : {"bytes", "max", "initial", "slots", "storequeue"})
        c.push_back({"P_config_" + k, [k] {
                         invalid([&] {
                             if (k == "storequeue") {
                                 ReadBlockPool pool(1, 2, ReadBlockFactory{64, 8});
                                 OutputQueue q(pool, 0); // Zero queue capacity is invalid.
                             } else {
                                 AllocNReleaseNSlicePool p(
                                     k == "bytes" ? 0 : 64, k == "initial" ? 2 : 0,
                                     k == "max" ? 0 : 1,
                                     {.max_slices_per_block = k == "slots" ? 0u : 1u});
                             }
                         });
                     }});
}
} // namespace ft
