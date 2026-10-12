#pragma once
#include <thread>
#include <utility>

// Tests and examples need scope-exit joining, without stop-token support.
// Apple libc++ shipped with some supported SDKs does not provide std::jthread.
namespace brq_support {
class JoiningThread {
    std::thread thread_;
public:
    JoiningThread() noexcept = default;
    template<class Function, class... Args>
    explicit JoiningThread(Function&& function, Args&&... args)
        : thread_(std::forward<Function>(function), std::forward<Args>(args)...) {}
    JoiningThread(JoiningThread&&) noexcept = default;
    JoiningThread& operator=(JoiningThread&& other) noexcept {
        if (this != &other) {
            if (thread_.joinable()) thread_.join();
            thread_ = std::move(other.thread_);
        }
        return *this;
    }
    ~JoiningThread() { if (thread_.joinable()) thread_.join(); }
    void join() { thread_.join(); }
    bool joinable() const noexcept { return thread_.joinable(); }
};
}
