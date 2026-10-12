#pragma once
#include "config.hpp"
#include <algorithm>

namespace TwoBufQueue::performance {
// Stop only at a naturally published record. Publication restarts on every
// source-block switch, so rounding the global record count is insufficient.
// Use the largest supported publish count that fits, keeping round boundaries
// identical across transports and publish-count candidates in the same profile.
class PublicationPlan {
public:
    const std::uint64_t messages, target_records, records_per_block;
    std::uint32_t alignment = 1;
    std::uint64_t warmup = 0;
    // 所有实现使用相同轮边界；TwoBufQueue 在计时外交接当前块，再等待本轮排空。
    const bool finish_blocks;

    PublicationPlan(const PerformanceConfig& config)
        : messages(config.messages), target_records(config.round_messages()),
          records_per_block(config.source.block_size/(config.payload_bytes)),
          finish_blocks((config.fixed_round_boundaries && config.round_storage_bytes) ||
                        !config.output.read_config.is_scan_current ||
                        config.publish_count == (std::numeric_limits<std::uint32_t>::max)()),
          rounds_(config.round_storage_bytes != 0) {
        if (!records_per_block) throw std::invalid_argument("source block must fit a record");
        if (finish_blocks) {
            warmup = std::max<std::uint64_t>(1,config.warmup_messages);
            return;
        }
        for (const auto limit : {1024u,512u,256u,128u,64u,32u,16u,8u,4u,2u,1u}) {
            // Round boundaries must also be published for counts such as 10.
            const auto candidate = limit - limit % config.publish_count;
            if (!candidate) continue;
            const auto required = (config.mode == MeasureMode::write_latency ? config.latency_batch_size : 1u)
                + 2ULL*(candidate-1);
            if (records_per_block >= candidate && (!config.round_storage_bytes || target_records >= required)) {
                alignment = candidate; break;
            }
        }
        if (config.publish_count > alignment || !records_per_block)
            throw std::invalid_argument("without active flush, a block and round must fit a publication group (and latency group)");
        const auto requested = std::max<std::uint64_t>(1,config.warmup_messages);
        const auto position = (requested-1)%records_per_block+1;
        const auto rounded = (position+alignment-1)/alignment*alignment;
        warmup = requested-position+(rounded <= records_per_block ? rounded : records_per_block+alignment);
    }

    [[nodiscard]] std::uint64_t next_end(std::uint64_t sent) const noexcept {
        if (messages-sent <= target_records) return messages; // finish publishes the final tail.
        if (finish_blocks) return sent+target_records;
        const auto desired = warmup+sent+target_records;
        const auto position = (desired-1)%records_per_block+1;
        const auto block_begin = desired-position;
        const auto published = position >= alignment
            ? block_begin+position/alignment*alignment
            : block_begin-records_per_block%alignment;
        return published-warmup;
    }
    [[nodiscard]] std::uint64_t minimum_round_records() const noexcept {
        if (finish_blocks) return target_records;
        return rounds_ ? target_records-2ULL*(alignment-1) : target_records;
    }
private:
    const bool rounds_;
};
} // namespace TwoBufQueue::performance
