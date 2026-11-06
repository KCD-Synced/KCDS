# Backend/replication/ — data-oriented world, schema, interest, snapshot/delta (build-order step 4).
add_library(kcd2s_replication STATIC world.cpp entity_lookup.cpp field_codec.cpp snapshot.cpp interest.cpp)
add_library(kcd2s::replication ALIAS kcd2s_replication)

target_include_directories(kcd2s_replication PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcd2s_replication PUBLIC kcd2s::core kcd2s::flags)

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME replication.schema TARGET test_repl_schema
                 SOURCES tests/schema_test.cpp
                 LIBS kcd2s::replication)
  kcd2s_add_test(NAME replication.entity_lookup TARGET test_repl_entity_lookup
                 SOURCES tests/entity_lookup_test.cpp
                 LIBS kcd2s::replication)
  kcd2s_add_test(NAME replication.world TARGET test_repl_world
                 SOURCES tests/world_test.cpp
                 LIBS kcd2s::replication)
  kcd2s_add_test(NAME replication.snapshot TARGET test_repl_snapshot
                 SOURCES tests/snapshot_test.cpp tests/update_cache_test.cpp tests/row_membership_test.cpp
                 LIBS kcd2s::replication)
  # Explicit benchmark target; timing is evidence, never a pass/fail CI threshold.
  add_executable(bench_repl_snapshot tests/snapshot_cache_bench.cpp)
  target_link_libraries(bench_repl_snapshot PRIVATE kcd2s::replication)
  kcd2s_add_test(NAME replication.interest TARGET test_repl_interest
                 SOURCES tests/interest_test.cpp
                 LIBS kcd2s::replication)
endif()
