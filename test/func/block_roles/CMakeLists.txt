add_executable(brq_block_roles tests.cpp)
target_link_libraries(brq_block_roles PRIVATE TwoBufQueue)
brq_warnings(brq_block_roles)
foreach(case interfaces mixed_batch reader_single reader_1n reader_nn writer_publish writer_trim_restore
    writer_scan_release reader_finish_with_held_slice)
  add_test(NAME block_roles_${case} COMMAND brq_block_roles ${case})
  set_tests_properties(block_roles_${case} PROPERTIES TIMEOUT 90 LABELS "functional;data_integrity;block_roles")
endforeach()
