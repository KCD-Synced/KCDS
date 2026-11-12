# Backend/core/ — platform-agnostic primitives: bit I/O, ids, quantization, threading (build-order step 2).
find_package(Threads REQUIRED)

add_library(kcd2s_core STATIC version.cpp ids.cpp threading.cpp crypto/sha256.cpp)
add_library(kcd2s::core ALIAS kcd2s_core)

target_include_directories(kcd2s_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcd2s_core PUBLIC kcd2s::flags Threads::Threads)

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME core.bits TARGET test_core_bits
                 SOURCES tests/bits_test.cpp
                 LIBS kcd2s::core)
  kcd2s_add_test(NAME core.varint_seq TARGET test_core_varint_seq
                 SOURCES tests/varint_seq_test.cpp
                 LIBS kcd2s::core)
  kcd2s_add_test(NAME core.ids TARGET test_core_ids
                 SOURCES tests/ids_test.cpp
                 LIBS kcd2s::core)
  kcd2s_add_test(NAME core.quant TARGET test_core_quant
                 SOURCES tests/quant_test.cpp
                 LIBS kcd2s::core)
  kcd2s_add_test(NAME core.threading TARGET test_core_threading
                 SOURCES tests/threading_test.cpp
                 LIBS kcd2s::core)
  kcd2s_add_test(NAME core.sha256 TARGET test_core_sha256
                 SOURCES tests/sha256_test.cpp
                 LIBS kcd2s::core)
  kcd2s_add_test(NAME core.float_chars TARGET test_core_float_chars
                 SOURCES tests/float_chars_test.cpp
                 LIBS kcd2s::core)
endif()
