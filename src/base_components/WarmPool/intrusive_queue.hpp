#pragma once

#include <cstddef>

namespace TwoBufQueue {

// 单线程侵入式 FIFO 队列：不分配内存、不拥有节点，链接由 next_member 提供。
// 节点必须非空且有效，同一链接不能同时用于其他容器，也不能重复入队。
// 跨线程使用时，所有访问须由同一把外部锁保护。
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
