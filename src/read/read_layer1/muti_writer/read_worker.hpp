#pragma once

#include "TwoBufQueue/src/read/read_layer1/types.hpp"
#include "TwoBufQueue/src/utils/auto_reset_event.hpp"
#include "TwoBufQueue/src/utils/brqHelp.hpp"
#include "TwoBufQueue/src/read/read_layer1/muti_writer/read_service.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>

namespace TwoBufQueue {

// 只负责线程、唤醒和停止；读取流程全部由 ReadService<Callback> 执行。
template<class Callback>
class ReadWorker {
public:
    // 构造参数 config 传入全部扫描/等待/收尾时长；默认值均可由调用方覆盖。
    // 保存配置但不启动线程，写入器必须在 start 前全部注册完成。
    // callback 由调用方持有，寿命必须覆盖 Worker；无需继承接口。
    explicit ReadWorker(ReadConfig config, Callback& callback);
    // 停止并回收内部线程；快退/超时后，析构前须重启排空或 take_pending 转交未完成记录。
    ~ReadWorker() noexcept;

    // 禁止复制读取线程及其绑定的写入器状态。
    ReadWorker(const ReadWorker&) = delete;
    // 禁止复制赋值，避免同一完成回调被多个工作对象接管。
    ReadWorker& operator=(const ReadWorker&) = delete;

    // 注册任意发布次数的 Writer 写入器，只允许在启动前调用。
    // WriterT 接受 Writer（或其 WriterBase 派生），只用公共只读接口。
    template<class WriterT>
    [[nodiscard]] bool add_writer(WriterT* writer) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return service_.add_writer(writer, running_.load(std::memory_order_acquire));
    }
    [[nodiscard]] bool add_queue(WriterQueue* queue, std::int32_t thread_id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return service_.add_queue(queue, thread_id, running_.load(std::memory_order_acquire));
    }
    // 显式处理空写入器重载并返回失败，便于统一调用方错误路径。
    [[nodiscard]] bool add_writer(std::nullptr_t) noexcept;
    // 校验状态、安装完成回调并创建后台读取线程；重复启动视为成功。
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
    // 返回启动前注册的写入器数量。
    [[nodiscard]] std::size_t writer_count() const noexcept;
    // 返回尚未处理的记录数：队内记录（含开始记录）+ Service 自持的 pending。
    [[nodiscard]] std::size_t pending_finished_count() const noexcept;

    // 停止并 join 后转交 pending，或在启动前接管另一个读端的 pending。
    [[nodiscard]] WriterQueue::Record take_pending(WriterQueue* queue) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) return {};
        return service_.take_pending(queue);
    }
    [[nodiscard]] bool adopt_pending(WriterQueue* queue, WriterQueue::Record& record) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) return false;
        return service_.adopt_pending(queue, record);
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

    // 串行化注册、启动和停止；事件内部管理自己的等待同步。
    mutable std::mutex mutex_;
    // 先构造、后销毁事件，保证 Service 持有的事件引用在其整个生命周期内有效。
    AutoResetEvent work_event_;
    ReadService<Callback> service_;
    std::thread worker_thread_;
    // Worker 仅持有线程运行、通知与停止请求，各写入线程的保存状态由 Service 中的 Item 持有。
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> memory_retry_requested_{false};
    // stop/request_stop(fast) 置位；仅 start 清除，快退在 Item 操作之间中止收尾。
    std::atomic<bool> fast_stop_requested_{false};
};

template<class Callback>
ReadWorker<Callback>::ReadWorker(ReadConfig config, Callback& callback)
    : service_(config, callback, work_event_) {}

template<class Callback>
ReadWorker<Callback>::~ReadWorker() noexcept { stop(); }

template<class Callback>
bool ReadWorker<Callback>::add_writer(std::nullptr_t writer) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return service_.add_writer(writer, running_.load(std::memory_order_acquire));
}

// 安装全部换块回调并启动读取线程；线程创建失败时完整回滚安装状态。
template<class Callback>
bool ReadWorker<Callback>::start() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load(std::memory_order_acquire)) return true;
    if (!service_.valid_start_state()) {
        service_.record_error(ReadError::invalid_start_state);
        return false;
    }
    initialize_fast_epoch_clock();
    stop_requested_.store(false, std::memory_order_relaxed);
    fast_stop_requested_.store(false, std::memory_order_relaxed);
    memory_retry_requested_.store(false, std::memory_order_relaxed);
    work_event_.reset();
    service_.prepare_start();
    running_.store(true, std::memory_order_release);
    try {
        worker_thread_ = std::thread(&ReadWorker<Callback>::run, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        service_.detach_completed_callbacks();
        service_.record_error(ReadError::thread_creation_failed);
        return false;
    }
    return true;
}

// 按调用方选择的慢退或快退模式退出，然后等待线程结束并清除写入器回调。
template<class Callback>
void ReadWorker<Callback>::stop(ReadStopMode mode) noexcept {
    if (!running_.load(std::memory_order_acquire)) return;
    request_stop(mode);
    if (worker_thread_.joinable()) worker_thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    service_.detach_completed_callbacks();
    running_.store(false, std::memory_order_release);
}

// 先写停止条件再设置事件：已经休眠的线程会醒来，尚未等待的线程也不会漏掉信号。
template<class Callback>
void ReadWorker<Callback>::request_stop(ReadStopMode mode) noexcept {
    if (!running_.load(std::memory_order_acquire)) return;
    if (mode == ReadStopMode::fast)
        fast_stop_requested_.store(true, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    notify_worker();
}

// 下游释放空间后设置事件；即使读取线程尚未等待，信号也会保留。
template<class Callback>
void ReadWorker<Callback>::notify_memory_available() noexcept {
    memory_retry_requested_.store(true, std::memory_order_release);
    notify_worker();
}

// acquire 读取工作线程运行标志，提供控制面瞬时状态。
template<class Callback>
bool ReadWorker<Callback>::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

template<class Callback>
std::size_t ReadWorker<Callback>::writer_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return service_.writer_count();
}

template<class Callback>
std::size_t ReadWorker<Callback>::pending_finished_count() const noexcept {
    return service_.pending_finished_count();
}

template<class Callback>
ReadStats ReadWorker<Callback>::stats() const noexcept {
    return service_.stats();
}

template<class Callback>
ReadError ReadWorker<Callback>::error_code() const noexcept {
    return service_.error_code();
}

template<class Callback>
std::string ReadWorker<Callback>::last_error() const {
    return error_message(error_code());
}

// 主循环：保存完成块 → 扫描活动块 → 有限自旋 → 等待事件或超时。
template<class Callback>
void ReadWorker<Callback>::run() noexcept {
    //尝试绑定 CPU
    if (!pin_current_thread(service_.config().worker_cpu)) {
        service_.record_error(ReadError::cpu_affinity_failed);
    }
    bool scan_all = true;
    for (;!stop_requested_.load(std::memory_order_acquire);) {
        const bool retry_memory = memory_retry_requested_.exchange(false, std::memory_order_acq_rel);
        // 通知持续到来时也检查期限，避免未通知的来源长期得不到扫描。
        scan_all |= retry_memory || service_.next_scan_deadline() <= SteadyClock::duration::zero();
        const auto result = scan_all ? service_.read_once(retry_memory, stop_requested_)
                                     : service_.read_notified(stop_requested_);
        if (result.error != ReadError::none) return;
        // 本轮有进展且仍有积压时直接继续；背压无进展时通过事件/超时重试。
        if (result.progressed && result.pending) continue;

        // 短暂自旋，新工作到来则开始下一轮。
        if (spin_for_completed_work()) {
            scan_all = false;
            continue;
        }

        // 等待数据/空间/控制事件或超时；空闲时采用最近扫描期限。
        scan_all = !wait_for_work();
    }

    finish_remaining_work();
}

// 慢退在统一期限内收齐源数据；快退停止继续收集。外部存储负责数据发布。
template<class Callback>
void ReadWorker<Callback>::finish_remaining_work() noexcept {
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
template<class Callback>
bool ReadWorker<Callback>::spin_for_completed_work() noexcept {
    for (std::uint32_t spin = 0; spin < service_.config().idle_spin_count; ++spin) {
        if (stop_requested_.load(std::memory_order_acquire)) return true;
        if (work_event_.try_wait()) return true; //有数据来了则返回
        cpu_relax();
    }
    return false;
}

// 等待时长取配置的最小间隔与扫描剩余时长中的较大值；事件可提前结束等待。
template<class Callback>
bool ReadWorker<Callback>::wait_for_work() noexcept {
    // 事件内部检查停止条件；request_stop 在设置条件后 set，唤醒已阻塞的等待。
    const SteadyClock::duration minimum_wait = service_.config().work_wait_min_interval;
    return work_event_.wait_for(std::max(minimum_wait, service_.next_scan_deadline()),
                               stop_requested_);
}

template<class Callback>
void ReadWorker<Callback>::notify_worker() noexcept {
    work_event_.set();
}


}  // namespace TwoBufQueue
