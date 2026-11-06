# Backend/voice/: the voice relay, the link to a standalone
# voice server, and that server: kcds-voice.
add_library(kcd2s_voice_cipher STATIC third_party/monocypher/monocypher.c)
target_include_directories(kcd2s_voice_cipher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(kcd2s_voice STATIC
  relay.cpp frame.cpp voice_server.cpp control_link.cpp voice_tenants.cpp net.cpp remote_relay.cpp
  voice_daemon.cpp)
add_library(kcd2s::voice ALIAS kcd2s_voice)

target_include_directories(kcd2s_voice PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcd2s_voice PUBLIC kcd2s::core kcd2s::replication kcd2s::flags
                      PRIVATE kcd2s_voice_cipher)
if(WIN32)
  target_link_libraries(kcd2s_voice PRIVATE ws2_32)
endif()

# The standalone voice server.
add_executable(kcds-voice server/main.cpp)
target_link_libraries(kcds-voice PRIVATE kcd2s::voice kcd2s::sdk)
# One binary for any Linux: its C++ runtime inside it, as the game server has it.
if(NOT WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_options(kcds-voice PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME voice.relay TARGET test_voice_relay
                 SOURCES tests/relay_test.cpp
                 LIBS kcd2s::voice)
  kcd2s_add_test(NAME voice.frame TARGET test_voice_frame
                 SOURCES tests/frame_test.cpp
                 LIBS kcd2s::voice)
  kcd2s_add_test(NAME voice.server TARGET test_voice_server
                 SOURCES tests/voice_server_test.cpp
                 LIBS kcd2s::voice)
  kcd2s_add_test(NAME voice.control TARGET test_voice_control
                 SOURCES tests/control_link_test.cpp
                 LIBS kcd2s::voice)
  kcd2s_add_test(NAME voice.daemon TARGET test_voice_daemon
                 SOURCES tests/voice_daemon_test.cpp
                 LIBS kcd2s::voice)
  # Explicit load bench, never a pass or fail threshold.
  add_executable(voice_relay_bench bench/voice_relay_bench.cpp)
  target_link_libraries(voice_relay_bench PRIVATE kcd2s::voice)
  add_executable(voice_udp_bench bench/voice_udp_bench.cpp)
  target_link_libraries(voice_udp_bench PRIVATE kcd2s::voice)
endif()
