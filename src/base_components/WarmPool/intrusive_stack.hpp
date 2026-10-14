#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace TwoBufQueue {

// Single-threaded intrusive LIFO: nodes supply a link through next_member.
// Does not own nodes; nodes must stay valid while stacked. Protect every cross-thread access with the same external lock.
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

// Atomic intrusive LIFO with separately serialized push/pop sides; the two sides may run concurrently.
// Each side may be one thread or several threads serialized by a common external lock that establishes happens-before.
// Nodes must live throughout their time in the stack and supply a non-atomic next_member link.
// Multiple concurrent pushes or concurrent pops are unsupported. Remove a node and return ownership externally
// before pushing it again; per-side serialization and ownership constraints exclude harmful ABA/link races.
template <typename Node, Node* Node::*next_member>
class alignas(64) SpscIntrusiveStack {
public:
    // The head uses a single atomic pointer; target platforms must provide genuinely lock-free atomic pointers.
    static_assert(std::is_object_v<Node>,
                  "SpscIntrusiveStack Node must be an object type");
    static_assert(std::atomic<Node*>::is_always_lock_free,
                  "SpscIntrusiveStack requires lock-free atomic pointers");

    // Create an empty stack; node allocation and destruction belong to the caller.
    constexpr SpscIntrusiveStack() noexcept = default;

    // Concurrent state is not copyable: two stacks must not own the same node chain.
    SpscIntrusiveStack(const SpscIntrusiveStack&) = delete;
    // Disable copy assignment to keep every node in exactly one stack at any time.
    SpscIntrusiveStack& operator=(const SpscIntrusiveStack&) = delete;

    // The serialized push side publishes a non-null, exclusively owned node to the head.
    void push(Node* node) noexcept {
        Node* observed = head_.load(std::memory_order_relaxed);
        do {
            node->*next_member = observed;
        } while (!head_.compare_exchange_weak(
            observed, node, std::memory_order_release,
            std::memory_order_relaxed));
    }

    // The serialized pop side acquires the most recently pushed node; an empty stack returns nullptr immediately.
    [[nodiscard]] Node* pop() noexcept {
        Node* observed = head_.load(std::memory_order_acquire);
        while (observed != nullptr) {
            Node* const next = observed->*next_member;
            if (head_.compare_exchange_weak(
                    observed, next, std::memory_order_acquire,
                    std::memory_order_acquire)) {
                // Clear the intrusive link after pop to aid diagnostics and mark the node as detached.
                observed->*next_member = nullptr;
                return observed;
            }
        }
        return nullptr;
    }

    // Diagnostic snapshot only; concurrent push/pop can invalidate the observation immediately.
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == nullptr;
    }

private:
    // Both sides modify the head; dedicate a cache line to avoid disturbing other control fields around this stack.
    alignas(64) std::atomic<Node*> head_{nullptr};
};

} // namespace TwoBufQueue
