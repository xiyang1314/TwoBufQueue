#pragma once

#include "TwoBufQueue/src/write/writer_base.hpp"
#include <mutex>

namespace TwoBufQueue {

// Retain the concrete callback type; Writer calls a templated member through the context pointer so user callbacks can inline.
// Same per-record callback signatures as ordinary / SeqWriter; no extra lock or context argument.
template<class TCallback>
class SyncContext {
public:
    using CallbackType = TCallback;
    template<class... TArgs>
    static constexpr bool accepts_record = [] {
        if constexpr (!std::is_nothrow_invocable_v<TCallback&, TArgs...>) return false;
        else return std::is_same_v<std::invoke_result_t<TCallback&, TArgs...>, void>;
    }();

    // TwoBufQueue selects this from Writer count during construction; the locking policy stays fixed after binding.
    explicit SyncContext(TCallback callback, bool need_lock = false)
        : callback_(std::move(callback)), need_lock_(need_lock) {}
    SyncContext(const SyncContext&) = delete;
    SyncContext& operator=(const SyncContext&) = delete;

    template<class... TArgs>
    FAST_LOG_ALWAYS_INLINE void invoke(TArgs&&... args) noexcept {
        static_assert(accepts_record<TArgs...>,
                      "sync callback must accept the record arguments, return void and be noexcept");
        std::unique_lock lock(mutex_, std::defer_lock);
        if (need_lock_) lock.lock();
        callback_(std::forward<TArgs>(args)...);
    }

private:
    [[no_unique_address]] TCallback callback_;
    const bool need_lock_;
    std::mutex mutex_;
};

namespace detail {
// Asynchronous Writer uses an empty base, with no sync pointer or extra hot-path branch.
template<class TContext>
class SyncBinding {
public:
    using SyncContextType = TContext;
    [[nodiscard]] bool has_sync_callback() const noexcept { return sync_context_ != nullptr; }
    // Bind only at queue construction; unbind only after every producer thread has stopped.
    void set_sync_callback(TContext& context) noexcept {
        assert(sync_context_ == nullptr);
        sync_context_ = &context;
    }
    void clear_sync_callback([[maybe_unused]] TContext& context) noexcept {
        assert(sync_context_ == &context);
        sync_context_ = nullptr;
    }
protected:
    TContext* sync_context_ = nullptr;
};
template<> class SyncBinding<void> {
public:
    using SyncContextType = void;
};
} // namespace detail
} // namespace TwoBufQueue
