#pragma once

#include <cstddef>

namespace TwoBufQueue {

// Single-threaded intrusive FIFO: no allocation or node ownership; next_member supplies the link.
// Nodes must be non-null and valid; do not share a link with another container or enqueue it twice.
// Cross-thread use requires all accesses to share one external lock.
template<class Node, Node* Node::*next_member>
class SingleThreadQueue {
public:
    SingleThreadQueue() noexcept = default;
    SingleThreadQueue(const SingleThreadQueue&) = delete;
    SingleThreadQueue& operator=(const SingleThreadQueue&) = delete;

    void push(Node* node) noexcept {
        node->*next_member = nullptr;
        if (tail_) tail_->*next_member = node;
        else head_ = node;
        tail_ = node;
        ++size_;
    }

    [[nodiscard]] Node* pop() noexcept {
        auto* node = head_;
        if (node) {
            head_ = node->*next_member;
            if (!head_) tail_ = nullptr;
            node->*next_member = nullptr;
            --size_;
        }
        return node;
    }

    [[nodiscard]] Node* front() const noexcept { return head_; }
    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    Node* head_ = nullptr;
    Node* tail_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace TwoBufQueue
