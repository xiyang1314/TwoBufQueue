add_executable(brq_source_lifecycle lifecycle.cpp)
target_link_libraries(brq_source_lifecycle PRIVATE TwoBufQueue)
brq_warnings(brq_source_lifecycle)
# Fast source blocks are borrowed until callback acceptance. Independent WriterBlock
# Slice lifecycle/death coverage remains in block_lifecycle and block_roles.
foreach(case finished_before_read unread_tail consume_before_handoff empty_finished claimed_generation
    callback_reuse rejected_finished finish_read_race queued_finish_reuse write_finished_holds_block
    empty_interleaved repeated_current empty_callback direct_handoff current_handoff
    buffered_retry_without_release_notify discard_unread_tail)
  add_test(NAME source_lifecycle_${case} COMMAND brq_source_lifecycle ${case})
  set_tests_properties(source_lifecycle_${case} PROPERTIES TIMEOUT 120 LABELS "functional;data_integrity;source_lifecycle")
endforeach()
