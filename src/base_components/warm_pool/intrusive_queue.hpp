#pragma once

#include <cstddef>

namespace TwoBufQueue {

// Single-threaded intrusive FIFO: no allocation or node ownership; next_member supplies the link.
// Nodes must be non-null and valid; do not share a link with another container or enqueue it twice.
// Cross-thread use requires all accesses to share one external lock.
template<class TNode, TNode* TNode::*TNextMember>
class SingleThreadQueue {
public:
    SingleThreadQueue() noexcept = default;
    SingleThreadQueue(const SingleThreadQueue&) = delete;
    SingleThreadQueue& operator=(const SingleThreadQueue&) = delete;

    void push(TNode* node) noexcept {
        node->*TNextMember = nullptr;
        if (tail_) tail_->*TNextMember = node;
        else head_ = node;
        tail_ = node;
        ++size_;
    }

    [[nodiscard]] TNode* pop() noexcept {
        auto* node = head_;
        if (node) {
            head_ = node->*TNextMember;
            if (!head_) tail_ = nullptr;
            node->*TNextMember = nullptr;
            --size_;
        }
        return node;
    }

    [[nodiscard]] TNode* front() const noexcept { return head_; }
    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    TNode* head_ = nullptr;
    TNode* tail_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace TwoBufQueue
