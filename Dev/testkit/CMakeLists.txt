# In-house test harness. Test executables link kcd2s::testkit (provides main()).
add_library(kcd2s_testkit STATIC testkit_main.cpp)
add_library(kcd2s::testkit ALIAS kcd2s_testkit)

target_include_directories(kcd2s_testkit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcd2s_testkit PUBLIC kcd2s::flags)

# Helper: register a test executable from sources, link testkit, add to CTest.
#   kcd2s_add_test(NAME <ctest-name> TARGET <exe> SOURCES a.cpp b.cpp LIBS kcd2s::core ...)
function(kcd2s_add_test)
  cmake_parse_arguments(T "" "NAME;TARGET" "SOURCES;LIBS" ${ARGN})
  add_executable(${T_TARGET} ${T_SOURCES})
  target_link_libraries(${T_TARGET} PRIVATE kcd2s::testkit ${T_LIBS})
  add_test(NAME ${T_NAME} COMMAND ${T_TARGET})
endfunction()
