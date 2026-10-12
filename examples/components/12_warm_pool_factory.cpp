// WarmPool 的自定义工厂：对象如何被创建和销毁。
// 默认工厂只是 new/delete；当对象需要特殊构造（比如要分配额外资源、要打标记）时，
// 传入自定义 Factory，池在需要新对象时调用 create()，销毁时调用 dispose()。

#include "TwoBufQueue/src/base_components/WarmPool/warm_pool.hpp"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
using namespace TwoBufQueue;

// 资源带一个自增 id，方便验证"复用"还是"新建"。
struct Connection {
    int id = 0;
    bool reset(std::uint64_t) noexcept { return true; } // 连接本身无需清理，直接复用
};

// 自定义工厂：create() 负责 new + 初始化，dispose() 负责完整销毁。
// dispose 必须 noexcept；create/dispose 都在池锁之外执行。
struct ConnectionFactory {
    int next_id = 0;
    Connection* create() {
        auto* conn = new Connection;
        conn->id = ++next_id;        // 每次新建给一个递增的 id
        return conn;
    }
    void dispose(Connection* conn) const noexcept { delete conn; }
};

int main() try {
    //--------------- 1. 初始化：用自定义工厂构造池 -----------------------------
    // 第 3 个参数是工厂实例；第 4 个是"空闲上限"，超过它归还时会回收对象。
    WarmPool<Connection, true, ConnectionFactory> pool(2, 4, ConnectionFactory{}, 2);

    //--------------- 2. 借出两个：工厂新建出 id=1、id=2 -----------------------
    auto a = pool.get();
    auto b = pool.get();
    if (!a || !b) throw std::runtime_error("get failed");
    if (pool.in_use_count() != 2) throw std::runtime_error("in_use_count mismatch");

    //--------------- 3. 归还后再次借出：应该复用旧对象而不是新建 ---------------
    if (!a.finish() || !b.finish()) throw std::runtime_error("recycle failed");

    auto c = pool.get();
    // id > 2 说明工厂又新建了一个对象（id=3）；复用的话 id 只会是 1 或 2。
    if (!c || c->id > 2) throw std::runtime_error("expected a reused object, not a new one");
    if (pool.resource_count() != 2) throw std::runtime_error("resource should not grow");

    if (!c.finish()) throw std::runtime_error("recycle failed");
    if (!pool.all_released()) throw std::runtime_error("not all released");

    std::puts("PASS: custom factory creates, pool reuses objects");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
