// Writer / TwoBufQueue parameters: specify every template argument, constructor argument and configuration field.
// The main thread writes two records; TwoBufQueue's copy and callback threads run independently.
#include "../example_support.hpp"
#include <chrono>
using namespace TwoBufQueue;
using namespace std::chrono_literals;

int main() try {
    // 1. Writer<TPublishWritePtrCount, TIsWriteLen, TDataLen>
    // TPublishWritePtrCount = 4: publish after every four successful writes; 1 publishes each record. Must be positive.
    // flush() publishes a partial group; finish() and block switches also publish remaining data.
    // The uint32_t maximum publishes only on flush / block switch / finish; write does not decrement a publication counter.
    // Combine with is_scan_current = false below to reduce producer hot-path overhead.
    // TIsWriteLen = true adds a length prefix; false writes raw bytes and requires TwoBufQueueBatch.
    // TDataLen = uint32_t selects a four-byte prefix and the write_mem length argument's type.
    // Choose an unsigned integer type; payload length must fit it and the whole prefixed record must fit one source block.
    constexpr std::uint32_t publish_write_ptr_count = 4;
    constexpr bool is_write_len = true;
    using DataLen = std::uint32_t;
    using MyWriter = Writer<publish_write_ptr_count, is_write_len, DataLen>;

    // 2. All WriterSourceConfig fields: source pool, source queue, source id and backpressure.
    const WriterSourceConfig source{
        .block_size = 4096,       // Bytes per source block, including record length prefixes.
        .pool_min = 2,            // Initially preallocated source blocks: 0 <= pool_min <= pool_max.
        .pool_max = 8,            // Maximum source block count, not message count; must be positive.
        .thread_id = 7,           // Nonnegative source identifier, not an OS thread id; Writer ids must be unique.
        .queue_capacity = 16,     // Source handoff-record capacity, including begin records; at least 2, independent of pool capacity.
        .backpressure = WriterBackpressureConfig{
            .mode = BackpressureMode::Block, // Wait when the next block is unavailable; Drop rejects the write immediately.
            .spin_count = 100,               // Block mode retries with pause this many times before timed waits.
            .wait_get_timeout = 2s,          // Total wait limit, including spinning; write returns false on timeout. Must be positive.
        },
    };

    // 3. Construct the source pool, source queue and Writer explicitly; all must outlive TwoBufQueue.
    // WriterPool(min_count, max_count, factory, max_free_count): the last argument caps retained free blocks.
    constexpr std::size_t max_free_count = 8;
    WriterPool pool(source.pool_min, source.pool_max,
                    SpscBlockFactory<SpscWriteBlock>{source.block_size}, max_free_count);
    // make_writer_queue(pool, capacity, enable_stats): true records reasons for queue acquisition failure.
    constexpr bool enable_stats = true;
    auto source_queue = make_writer_queue(pool, source.queue_capacity, enable_stats);
    // Writer(queue, thread_id, backpressure): supply all three constructor arguments.
    MyWriter writer(&source_queue, source.thread_id, source.backpressure);
    // To avoid manually managing the source pool and queue, replace these three objects with:
    // auto writer = make_writer<MyWriter>(source); // Explicit WriterT; every source field is supplied above.
    // The resulting type is OwnedWriter<MyWriter>; use decltype(writer) as the queue's WriterT.

    // 4. All TwoBufQueueOptions fields: output resources and readers; their capacities are independent of source.
    const TwoBufQueueOptions options{
        .output_block_size = 8192,    // Data bytes per output block; must hold the largest readable source-block range.
        .output_frame_capacity = 16,  // Maximum source ranges per output block; one frame can contain multiple records.
        .output_pool_min = 2,         // Initial output block count: 0 <= min <= max.
        .output_pool_max = 8,         // Maximum output block count; must be positive.
        .output_queue_capacity = 8,   // Output handoff queue slots; positive and independent of the output pool limit.
        .read_layer1_thread_count = 1,// Reader count, clamped to [1, Writer count]; assign Writers round-robin.
        .read_config = ReadConfig{
            .idle_spin_count = 100,          // Poll with pause this many times when no new completed block exists; zero waits immediately.
            .idle_rescan_interval = 1ms,     // Initial current-block scan interval; must be positive.
            .worker_cpu = -1,                // -1 disables CPU binding; nonnegative values select a logical CPU shared by all readers.
            .idle_rescan_max_interval = 16ms,// Maximum exponentially increased interval after consecutive empty scans; at least the initial interval.
            .finish_timeout = 2s,            // Total deadline for collecting source data during graceful stop; must be positive.
            .work_wait_min_interval = 1ms,   // Minimum ordinary wait; an event may wake it earlier. Must be positive.
            .finish_retry_interval = 1ms,    // Retry interval while graceful stop still has unread data; must be positive.
            .is_scan_current = true,         // false reads completed blocks only; tails arrive after a block switch or finish.
        },
        .cancel_claim_mode = CancelClaimMode::Keep, // Claim available blocks in original enqueue order; QueueTail requeues cancelled claims at the tail.
        .output_mode = HandoffMode::MPSC,           // Independent callback thread; multiple copy threads may share the output queue.
        .output_reader_mode = ReaderMode::WaitSignal, // Wait for a notification when empty; Polling continuously checks for work.
        .output_reader_count = 1,                  // Must be 1 for SPSC/MPSC; SPMC/MPMC allow 1..64.
        .output_retry_wait = 1ms,                  // WaitSignal retry delay after a rejected callback.
        .output_enable_stats = true,              // Enable output handoff queue statistics.
    };
    // Data resets the scan interval. Update scan timing when scanning, regardless of the callback's result.
    // flush / block switch / finish notifications wake only that source: read completed blocks, then check current immediately.
    // With is_scan_current = false, neither notifications nor timeouts check current; finish every Writer before stopping the queue.
    // flush does not wait for consumption. Scan timeouts visit all sources; space notifications force retries for all sources.
    // finish_timeout cannot interrupt a running business callback; callbacks must return promptly.
    // cancel_claim_mode affects the output HandoffQueue, not the Writer's ordered source queue.
    // Keep allows claimed blocks to run concurrently without ordered completion; HandoffOptions uses the same field name.
    // Per-record callbacks ignore return values and do not normally cancel; a false TwoBufQueueBatch callback retries using this mode.

    // 5. TwoBufQueue<RecordCallback, WriterT>: supply both template arguments explicitly.
    // RecordCallback is the callback type; WriterT is the common type of every Writer passed to this queue.
    // Ordinary Writer callback arguments: source id, payload address and payload bytes, excluding the length prefix.
    // The callback must be noexcept; this per-record interface ignores its return value. data is valid only during the callback.
    // Callback that receives messages.
    auto on_record = [](std::int32_t thread_id, const std::byte* data, std::size_t size) noexcept {
        std::printf("writer=%d: %.*s\n", static_cast<int>(thread_id),
                    static_cast<int>(size), reinterpret_cast<const char*>(data));
    };
    // Single-Writer construction: TwoBufQueue(callback, writer reference, options); construction starts the reader.
    ::TwoBufQueue::TwoBufQueue<decltype(on_record), MyWriter> queue(on_record, writer, options);
    // For multiple Writers, replace only the second argument: queue(on_record, {&writer1, &writer2}, options).
    // Each Writer owns its source pool/queue, is used by one producer and has a distinct thread_id.
    // read_layer1_thread_count > 1 copies multiple Writers in parallel; independent threads invoke output callbacks.
    // A non-void TSyncContext enables per-record callbacks in the Writer/SeqWriter's producer thread; see 06_sync_writer.cpp.
    example::require(queue.started(), "reader failed to start");

    // 6. Write only two records and explicitly publish before reaching four; finish the Writer, then drain the queue.
    constexpr char message[] = "hello";
    constexpr DataLen message_size = sizeof(message) - 1; // Exclude the terminating '\0'.
    const bool first = writer.write_mem(message, message_size);
    const bool second = writer.write('o', 'k');
    writer.flush();
    writer.finish();
    queue.stop();
    example::require(first && second && queue.error_code() == ReadError::none, "write/read failed");
    std::puts("PASS: all Writer and TwoBufQueue parameters specified");
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
