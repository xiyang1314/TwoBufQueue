#pragma once
#include "TwoBufQueue/src/base_components/warm_pool/resource.hpp"
#include <cstdio>
#include <exception>
#include <source_location>

namespace TwoBufQueue {
// Static diagnostics for error paths only; owns no resources and manages neither pool identity nor lifetimes.
template<class TBlock> struct BlockDiagnostics {
    struct ErrorInfo {
        const char* reason;
        const TBlock* range;
        bool has_actual_state;
        std::uint8_t actual_state;
        const char* actual_state_name;
        std::source_location location;
    };
    using ErrorHandler = void (*)(const ErrorInfo&) noexcept;
    static void set_error_handler(ErrorHandler handler) noexcept {
        error_handler_.store(handler, std::memory_order_release);
    }
    [[noreturn]] static void report_error(const char* reason, const TBlock* block,
        std::source_location location = std::source_location::current(), bool inactive = false) noexcept {
        ErrorInfo info{reason, block, block != nullptr, 0, "unavailable", location};
        if (block && block->describe_error_) block->describe_error_(block, inactive, info);
        fail(info);
    }
    [[noreturn]] static void report_invalid(const char* reason,
        std::source_location location = std::source_location::current()) noexcept {
        fail({reason, nullptr, false, 0, "unavailable", location});
    }
private:
    [[noreturn]] static void fail(const ErrorInfo& info) noexcept {
        std::fprintf(stderr, "Memory pool fatal error: %s\n  at %s:%u (%s)\n  block=%p state=%s\n",
            info.reason, info.location.file_name(), static_cast<unsigned>(info.location.line()),
            info.location.function_name(), static_cast<const void*>(info.range), info.actual_state_name);
        std::fflush(stderr);
        if (const auto handler = error_handler_.load(std::memory_order_acquire)) handler(info);
        std::terminate();
    }
    inline static std::atomic<ErrorHandler> error_handler_{nullptr};
};
} // namespace TwoBufQueue
