// WarmPool：一个对象资源池。
// 池里的对象被重复使用，避免高频 new/delete；借出时对象先被 reset() 复位成"干净"状态，
// 用完归还，对象回到池里等待下次借出。适合消息对象、连接对象这类反复借还的场景。

#include "TwoBufQueue/src/base_components/WarmPool/warm_pool.hpp"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
using namespace TwoBufQueue;

// 放进池里的对象只需要实现 reset(generation)：每次借出前调用，把对象恢复成初始状态。
// 返回 false 表示这次借出失败（对象会被安排延迟销毁，本次拿不到）。
struct Message {
    int value = 0;
    bool reset(std::uint64_t) noexcept { value = 0; return true; }
};

int main() try {
    //--------------- 1. 初始化：至少 2 个、最多 8 个对象 -----------------------
    // 构造时就会立即预分配 min_count(2) 个对象，避免运行时才慢慢创建。
    WarmPool<Message> pool(2, 8);

    //--------------- 2. 借出：get() 返回一个 Loan（借条），不拥有对象 ---------
    auto loan = pool.get();
    if (!loan) throw std::runtime_error("get failed");   // 池满或创建失败时为空
    loan->value = 42;                                    // 像指针一样访问对象
    if (!pool.is_valid(loan)) throw std::runtime_error("loan should be valid");

    //--------------- 3. 归还：finish()（等价 recycle()）-----------------------
    // finish 是 [[nodiscard]]，必须检查返回值：true 表示归还成功。
    if (!loan.finish()) throw std::runtime_error("recycle failed");

    // 再次借出：对象会复用上一次归还的那个，reset() 已经把 value 清成 0。
    auto again = pool.get();
    if (!again || again->value != 0) throw std::runtime_error("reset not applied on reuse");
    if (!again.finish()) throw std::runtime_error("second recycle failed");

    //--------------- 4. 查询与预分配 ------------------------------------------
    if (!pool.all_released()) throw std::runtime_error("not all released");
    // 提前把在池对象数补齐到 4 个，减少后续 get() 的动态分配开销。
    if (!pool.preallocate(4)) throw std::runtime_error("preallocate failed");
    if (pool.resource_count() != 4) throw std::runtime_error("resource_count mismatch");

    std::puts("PASS: borrow, recycle, reuse and preallocate");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
