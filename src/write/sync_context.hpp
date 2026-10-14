#pragma once

#include "TwoBufQueue/src/write/writer_base.hpp"
#include <mutex>

namespace TwoBufQueue {

// Retain the concrete callback type; Writer calls a templated member through the context pointer so user callbacks can inline.
// Same per-record callback signatures as ordinary / SeqWriter; no extra lock or context argument.
template<class Callback>
class SyncContext {
public:
    using CallbackType = Callback;
    template<class... Args>
    static constexpr bool accepts_record = [] {
        if constexpr (!std::is_nothrow_invocable_v<Callback&, Args...>) return false;
        else return std::is_same_v<std::invoke_result_t<Callback&, Args...>, void>;
    }();

    // TwoBufQueue selects this from Writer count during construction; the locking policy stays fixed after binding.
    explicit SyncContext(Callback callback, bool need_lock = false)
        : callback_(std::move(callback)), need_lock_(need_lock) {}
    SyncContext(const SyncContext&) = delete;
    SyncContext& operator=(const SyncContext&) = delete;

    template<class... Args>
    FAST_LOG_ALWAYS_INLINE void invoke(Args&&... args) noexcept {
        static_assert(accepts_record<Args...>,
                      "sync callback must accept the record arguments, return void and be noexcept");
        std::unique_lock lock(mutex_, std::defer_lock);
        if (need_lock_) lock.lock();
        callback_(std::forward<Args>(args)...);
    }

private:
    [[no_unique_address]] Callback callback_;
    const bool need_lock_;
    std::mutex mutex_;
};

namespace detail {
// Asynchronous Writer uses an empty base, with no sync pointer or extra hot-path branch.
template<class Context>
class SyncBinding {
public:
    using SyncContextType = Context;
    [[nodiscard]] bool has_sync_callback() const noexcept { return sync_context_ != nullptr; }
    // Bind only at queue construction; unbind only after every producer thread has stopped.
    void set_sync_callback(Context& context) noexcept {
        assert(sync_context_ == nullptr);
        sync_context_ = &context;
    }
    void clear_sync_callback([[maybe_unused]] Context& context) noexcept {
        assert(sync_context_ == &context);
        sync_context_ = nullptr;
    }
protected:
    Context* sync_context_ = nullptr;
};
template<> class SyncBinding<void> {
public:
    using SyncContextType = void;
};
} // namespace detail
} // namespace TwoBufQueue
