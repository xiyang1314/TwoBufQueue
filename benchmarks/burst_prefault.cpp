// Benchmark-only replacement for aligned allocation. It prefaults raw buffers
// before object construction, never modifies a live object or a queue hot path.
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif
namespace { std::atomic<bool> prefault{false}; }
void burst_prefault_aligned_buffers(bool value) noexcept { prefault.store(value,std::memory_order_relaxed); }
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    const auto size = bytes ? bytes : 1;
    for (;;) {
        void* p = nullptr;
#if defined(_WIN32)
        p = _aligned_malloc(size,static_cast<std::size_t>(alignment));
#else
        if (posix_memalign(&p,static_cast<std::size_t>(alignment),size)) p = nullptr;
#endif
        if (p) {
            if (prefault.load(std::memory_order_relaxed)) std::memset(p,0,size);
            return p;
        }
        if (auto handler = std::get_new_handler()) handler(); else throw std::bad_alloc{};
    }
}
void operator delete(void* p, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p,a); }
