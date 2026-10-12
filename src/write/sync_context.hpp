#pragma once

#include "TwoBufQueue/src/write/writer_base.hpp"
#include <mutex>

namespace TwoBufQueue {

// 保存具体回调类型；Writer 通过对象指针调用模板成员，允许内联用户回调。
// 回调签名与普通 / SeqWriter 的逐条回调一致，不增加锁或 context 参数。
template<class Callback>
class SyncContext {
public:
    using CallbackType = Callback;
    template<class... Args>
    static constexpr bool accepts_record = [] {
        if constexpr (!std::is_nothrow_invocable_v<Callback&, Args...>) return false;
        else return std::is_same_v<std::invoke_result_t<Callback&, Args...>, void>;
    }();

    // TwoBufQueue 构造时根据 Writer 数量决定；绑定后不再改变锁策略。
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
// 异步 Writer 使用空基类，不携带同步指针，也不增加热路径分支。
template<class Context>
class SyncBinding {
public:
    using SyncContextType = Context;
    [[nodiscard]] bool has_sync_callback() const noexcept { return sync_context_ != nullptr; }
    // 仅在构造队列时绑定、停止所有写线程后解绑。
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
