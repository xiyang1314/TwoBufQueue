#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace TwoBufQueue {

// 单线程侵入式 LIFO 栈：节点通过 next_member 提供链接字段。
// 不拥有节点；节点在栈中时必须保持有效。跨线程使用时，所有访问须由同一把外部锁保护。
template<class Node, Node* Node::*next_member>
class SingleThreadStack {
public:
    void push(Node* node) noexcept {
        node->*next_member = head_;
        head_ = node;
    }

    Node* pop() noexcept {
        auto* node = head_;
        if (node) {
            head_ = node->*next_member;
            node->*next_member = nullptr;
        }
        return node;
    }

    bool empty() const noexcept { return head_ == nullptr; }

private:
    Node* head_ = nullptr;
};

// 压栈端、弹栈端各自串行的原子侵入式 LIFO 栈，两端之间允许并发。
// 每端可以是唯一线程，也可以是由共同外部锁串行化并建立 happens-before 的多个线程。
// Node 的生命周期必须覆盖其入栈时间，并通过 next_member 提供一个非原子链接字段。
// 本类不能直接供多个 push 或多个 pop 并发调用：节点必须先被取出并经外部所有权链路交还，
// 压栈端才能再次压入；每端串行及节点所有权约束排除了有害的 ABA/链接并发读写。
template <typename Node, Node* Node::*next_member>
class alignas(64) SpscIntrusiveStack {
public:
    // 栈顶使用单指针原子操作；项目要求目标平台上的原子指针真正无锁。
    static_assert(std::is_object_v<Node>,
                  "SpscIntrusiveStack Node must be an object type");
    static_assert(std::atomic<Node*>::is_always_lock_free,
                  "SpscIntrusiveStack requires lock-free atomic pointers");

    // 创建空栈；本类不拥有节点，也不负责节点的创建和销毁。
    constexpr SpscIntrusiveStack() noexcept = default;

    // 并发状态不能复制，否则两个栈会同时持有同一节点链。
    SpscIntrusiveStack(const SpscIntrusiveStack&) = delete;
    // 禁止复制赋值，保持每个节点在任意时刻只属于一个栈。
    SpscIntrusiveStack& operator=(const SpscIntrusiveStack&) = delete;

    // 由串行压栈端把非空节点发布到栈顶；节点入栈前必须由调用者独占。
    void push(Node* node) noexcept {
        Node* observed = head_.load(std::memory_order_relaxed);
        do {
            node->*next_member = observed;
        } while (!head_.compare_exchange_weak(
            observed, node, std::memory_order_release,
            std::memory_order_relaxed));
    }

    // 由串行弹栈端取得最近压入的节点；空栈立即返回 nullptr。
    [[nodiscard]] Node* pop() noexcept {
        Node* observed = head_.load(std::memory_order_acquire);
        while (observed != nullptr) {
            Node* const next = observed->*next_member;
            if (head_.compare_exchange_weak(
                    observed, next, std::memory_order_acquire,
                    std::memory_order_acquire)) {
                // 弹出后清除侵入链接，便于诊断并明确节点已脱离栈。
                observed->*next_member = nullptr;
                return observed;
            }
        }
        return nullptr;
    }

    // 只提供瞬时诊断结果；并发 push/pop 时返回值可能立即变化。
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == nullptr;
    }

private:
    // 两个线程共同修改栈顶，独占缓存行避免污染包含本栈的其他控制字段。
    alignas(64) std::atomic<Node*> head_{nullptr};
};

} // namespace TwoBufQueue
