#include "TwoBufQueue/src/base_components/WarmPool/warm_pool.hpp"

struct Item {
    int value = 0;
    bool reset(std::uint64_t) noexcept { value = 0; return true; }
};

int main() {
    TwoBufQueue::WarmPool<Item> pool(1, 2);
    auto loan = pool.get();
    if (!loan || !loan.finish()) return 1;
    return pool.all_released() && !pool.is_valid(loan) ? 0 : 2;
}
