add_executable(brq_warm_pool tests.cpp)
target_link_libraries(brq_warm_pool PRIVATE TwoBufQueue)
brq_warnings(brq_warm_pool)
foreach(case protocol resource lifecycle independent failures factory_dispose trim_destroy
    trim_recreate slow_hooks trim_overlap wait generation_limit concurrent
    dispose_lifecycle dispose_raw_memory dispose_ctor_failure duplicate_recycle stale_during_reset factory_loan default_finish node_lifetime node_concurrent deferred_two_candidates retired_record_reuse)
  add_test(NAME warm_pool_${case} COMMAND brq_warm_pool ${case})
  set_tests_properties(warm_pool_${case} PROPERTIES TIMEOUT 90 LABELS "functional;warm_pool;data_integrity")
endforeach()
add_test(NAME warm_pool_death_live COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../block_lifecycle/check_death.py"
  "$<TARGET_FILE:brq_warm_pool>" death_live "resources are still in use")
set_tests_properties(warm_pool_death_live PROPERTIES TIMEOUT 15 LABELS "functional;warm_pool;death")

add_executable(brq_resource_header_only header_only.cpp)
target_compile_features(brq_resource_header_only PRIVATE cxx_std_20)
target_include_directories(brq_resource_header_only PRIVATE "${CMAKE_BINARY_DIR}/include")
target_link_libraries(brq_resource_header_only PRIVATE Threads::Threads)
brq_warnings(brq_resource_header_only)
add_test(NAME warm_pool_header_only COMMAND brq_resource_header_only)
set_tests_properties(warm_pool_header_only PROPERTIES TIMEOUT 15 LABELS "functional;warm_pool")
