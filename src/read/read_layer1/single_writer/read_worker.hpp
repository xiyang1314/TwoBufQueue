#pragma once

#include "TwoBufQueue/src/read/read_layer1/types.hpp"
#include "TwoBufQueue/src/utils/auto_reset_event.hpp"
#include "TwoBufQueue/src/utils/brqHelp.hpp"
#include "TwoBufQueue/src/read/read_layer1/single_writer/read_service.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>

namespace TwoBufQueue {

// 单 Writer 的独立线程调度；事件本身即代表唯一来源，无需来源位图或额外重试标志。
template<class TCallback>
class SingleReadWorker {
public:
    // 构造时直接绑定唯一 Writer；不分配来源容器，不提供动态注册接口。
    // callback / writer 须覆盖 Worker 的生命周期。start/stop 时生产者须静止。
    SingleReadWorker(ReadConfig config, TCallback& callback, WriterQueue& queue, std::int32_t writer_id);
    template<class TWriter>
    SingleReadWorker(ReadConfig config, TCallback& callback, TWriter& writer)
        : SingleReadWorker(config, callback, writer.handoff_queue(), writer.thread_id()) {}
    ~SingleReadWorker() noexcept;
    SingleReadWorker(const SingleReadWorker&) = delete;
    SingleReadWorker& operator=(const SingleReadWorker&) = delete;

    // 校验状态、安装数据通知回调并创建后台读取线程；重复启动视为成功。
    [[nodiscard]] bool start() noexcept;

    // 停止前须停止生产者写入、完成 flush，并等待已有完成块回调返回。
    // is_scan_current=false 时须 finish Writer，使未完成的尾部进入完成队列。
    // slow（默认）：收齐剩余源数据并交给回调；等待下游最多 finish_timeout。
    // 超时记录 finish_timeout 并退出；stop 返回不再保证源数据已收齐。
    // fast：当前同步回调返回后停止收集，不等下游腾出空间。
    // 两种模式均等待读取线程退出并解除写入器回调。
    // 外部存储须随后自行 finish，并使用自身排空接口判断下游是否释放完。
    // 由控制线程串行调用，不能同时执行多个 stop。
    void stop(ReadStopMode mode = ReadStopMode::slow) noexcept;

    // 非阻塞停止请求：设置条件并唤醒普通等待或收尾等待，不 join、不解除回调。
    // 可在另一控制线程的 stop(slow) 期间调用 request_stop(fast) 中断收尾。
    // 快退请求不可被后续慢退降级；调用后仍须由一个控制线程 stop 完成回收。
    // 须与 start/析构互斥；请求慢退前同样须停止写入并完成 flush。
    void request_stop(ReadStopMode mode = ReadStopMode::slow) noexcept;

    // 归并线程释放批次后通知读取线程重试此前受背压的 SpscWriteBlock。
    void notify_memory_available() noexcept;

    // 返回后台读取线程是否已经成功启动且尚未完全停止。
    [[nodiscard]] bool running() const noexcept;
    // 返回尚未处理的记录数：队内记录（含开始记录）+ Service 自持的 pending。
    [[nodiscard]] std::size_t pending_finished_count() const noexcept;

    // 停止并 join 后转交 pending，或在启动前接管另一个读端的 pending。
    [[nodiscard]] WriterQueue::Record take_pending() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) return {};
        return service_.take_pending();
    }
    [[nodiscard]] bool adopt_pending(WriterQueue::Record& record) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) return false;
        return service_.adopt_pending(record);
    }
    // 汇总读取、游标确认及背压统计；存储统计由外部保存对象提供。
    [[nodiscard]] ReadStats stats() const noexcept;
    // 原子读取最近一次记录的错误；初始为 none，成功操作不会清除历史错误。
    [[nodiscard]] ReadError error_code() const noexcept;
    // 兼容文本接口：查询时将枚举转换为文本，不保存错误字符串。
    [[nodiscard]] std::string last_error() const;

private:
    using SteadyClock = FastSteadyClock;
    void run() noexcept;
    void finish_remaining_work() noexcept;
    [[nodiscard]] bool spin_for_completed_work() noexcept;
    [[nodiscard]] bool wait_for_work() noexcept;
    void notify_worker() noexcept;

    // 串行化启动和停止；事件内部管理自己的等待同步。
    mutable std::mutex mutex_;
    // 先构造、后销毁事件，保证 Service 持有的事件引用在其整个生命周期内有效。
    alignas(64) AutoResetEvent work_event_;
    alignas(64) SingleReadService<TCallback> service_;
    std::thread worker_thread_;
    // Worker 仅持有线程运行、通知与停止请求，唯一写入线程的读取状态由 Service 持有。
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    // stop/request_stop(fast) 置位；仅 start 清除，快退在 Item 操作之间中止收尾。
    std::atomic<bool> fast_stop_requested_{false};
};

template<class TCallback>
SingleReadWorker<TCallback>::SingleReadWorker(ReadConfig config, TCallback& callback,
                                             WriterQueue& queue, std::int32_t writer_id)
    : service_(config, callback, work_event_, queue, writer_id) {}

template<class TCallback>
SingleReadWorker<TCallback>::~SingleReadWorker() noexcept { stop(); }

// 安装唯一来源的回调并启动读取线程；线程创建失败时完整回滚安装状态。
template<class TCallback>
bool SingleReadWorker<TCallback>::start() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load(std::memory_order_acquire)) return true;
    if (!service_.valid_start_state()) {
        service_.record_error(ReadError::invalid_start_state);
        return false;
    }
    initialize_fast_epoch_clock();
    stop_requested_.store(false, std::memory_order_relaxed);
    fast_stop_requested_.store(false, std::memory_order_relaxed);
    work_event_.reset();
    service_.prepare_start();
    running_.store(true, std::memory_order_release);
    try {
        worker_thread_ = std::thread(&SingleReadWorker<TCallback>::run, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        service_.detach_completed_callbacks();
        service_.record_error(ReadError::thread_creation_failed);
        return false;
    }
    return true;
}

// 按调用方选择的慢退或快退模式退出，然后等待线程结束并清除写入器回调。
template<class TCallback>
void SingleReadWorker<TCallback>::stop(ReadStopMode mode) noexcept {
    if (!running_.load(std::memory_order_acquire)) return;
    request_stop(mode);
    if (worker_thread_.joinable()) worker_thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    service_.detach_completed_callbacks();
    running_.store(false, std::memory_order_release);
}

// 先写停止条件再设置事件：已经休眠的线程会醒来，尚未等待的线程也不会漏掉信号。
template<class TCallback>
void SingleReadWorker<TCallback>::request_stop(ReadStopMode mode) noexcept {
    if (!running_.load(std::memory_order_acquire)) return;
    if (mode == ReadStopMode::fast)
        fast_stop_requested_.store(true, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    notify_worker();
}

// 下游释放空间后设置事件；即使读取线程尚未等待，信号也会保留。
template<class TCallback>
void SingleReadWorker<TCallback>::notify_memory_available() noexcept {
    notify_worker();
}

// acquire 读取工作线程运行标志，提供控制面瞬时状态。
template<class TCallback>
bool SingleReadWorker<TCallback>::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

template<class TCallback>
std::size_t SingleReadWorker<TCallback>::pending_finished_count() const noexcept {
    return service_.pending_finished_count();
}

template<class TCallback>
ReadStats SingleReadWorker<TCallback>::stats() const noexcept {
    return service_.stats();
}

template<class TCallback>
ReadError SingleReadWorker<TCallback>::error_code() const noexcept {
    return service_.error_code();
}

template<class TCallback>
std::string SingleReadWorker<TCallback>::last_error() const {
    return error_message(error_code());
}

// 主循环：保存完成块 → 扫描活动块 → 有限自旋 → 等待事件或超时。
template<class TCallback>
void SingleReadWorker<TCallback>::run() noexcept {
    //尝试绑定 CPU
    if (!pin_current_thread(service_.config().worker_cpu)) {
        service_.record_error(ReadError::cpu_affinity_failed);
    }
    // 启动/重启时直接读取已有数据；之后只有事件绕过 current 扫描间隔。
    bool force_current = true;
    while (!stop_requested_.load(std::memory_order_acquire)) {
        // 在固定快照前合并已有事件；读取期间的新事件仍留给下一轮。
        // 包括有积压直接继续的情况，避免稍后因旧信号再空扫一次。
        force_current |= work_event_.try_wait();
        const auto result = service_.read_once(force_current, stop_requested_);
        if (result.error != ReadError::none) return;
        if (result.progressed && result.pending) continue;
        force_current = spin_for_completed_work();
        if (!force_current) force_current = wait_for_work();
    }

    finish_remaining_work();
}

// 慢退在统一期限内收齐源数据；快退停止继续收集。外部存储负责数据发布。
template<class TCallback>
void SingleReadWorker<TCallback>::finish_remaining_work() noexcept {
    using Clock = AutoResetEvent::Clock;
    const auto now = Clock::now();
    const auto timeout = service_.config().finish_timeout;
    const auto deadline = now >= Clock::time_point::max() - timeout
        ? Clock::time_point::max() : now + timeout;

    for (;;) {
        // stop_requested_ 在进入收尾前就已置位；收尾只响应新增的快退请求。
        if (fast_stop_requested_.load(std::memory_order_acquire)) {
            return;
        }
        if (Clock::now() >= deadline) {
            service_.record_error(ReadError::finish_timeout);
            return;
        }

        const auto result = service_.read_once(true, fast_stop_requested_, deadline);
        if (result.error != ReadError::none || fast_stop_requested_.load(std::memory_order_acquire) ||
            !result.pending) return;

        // 按构造配置重试；空间/停止事件会提前唤醒，不因通知而延长总期限。
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) continue;
        const Clock::duration retry_interval = service_.config().finish_retry_interval;
        (void)work_event_.wait_for(std::min(retry_interval, remaining), fast_stop_requested_);
    }
}

// 有限自旋检查停止请求并尝试消费事件，命中后回到主循环。
template<class TCallback>
bool SingleReadWorker<TCallback>::spin_for_completed_work() noexcept {
    for (std::uint32_t spin = 0; spin < service_.config().idle_spin_count; ++spin) {
        if (stop_requested_.load(std::memory_order_acquire)) return true;
        if (work_event_.try_wait()) return true; //有数据来了则返回
        cpu_relax();
    }
    return false;
}

// 等待时长取配置的最小间隔与扫描剩余时长中的较大值；事件可提前结束等待。
template<class TCallback>
bool SingleReadWorker<TCallback>::wait_for_work() noexcept {
    // 事件内部检查停止条件；request_stop 在设置条件后 set，唤醒已阻塞的等待。
    const SteadyClock::duration minimum_wait = service_.config().work_wait_min_interval;
    return work_event_.wait_for(std::max(minimum_wait, service_.next_scan_deadline()),
                               stop_requested_);
}

template<class TCallback>
void SingleReadWorker<TCallback>::notify_worker() noexcept {
    work_event_.set();
}


}  // namespace TwoBufQueue
