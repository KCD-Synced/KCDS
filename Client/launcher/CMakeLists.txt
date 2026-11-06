# Client/launcher/ — what the game client, the master client and the server share with the KCDS
# launcher: the bounded JSON reader, the server address and join-link rules, the origin rule and
# the native crash capture (kcds::launcher, kcds::launcher_sentry).
#
# The launcher itself (the updater window, the update feed, the installer and the self-update
# helper) is not open source. Official builds check it out from its own repository into
# Client/launcher/private/, which adds its targets here; without it the build has everything else.
add_library(kcds_launcher STATIC
  lib/json.cpp
  lib/origin.cpp
  crash/crash.cpp
  crash/handoff.cpp
  crash/logs.cpp
  crash/crash_test.cpp)
add_library(kcds::launcher ALIAS kcds_launcher)
target_include_directories(kcds_launcher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcds_launcher PUBLIC kcd2s::flags)
if(WIN32)
  target_sources(kcds_launcher PRIVATE
    crash/handoff_win.cpp
    crash/log_tail_win.cpp
    crash/crash_test_win.cpp)
  target_link_libraries(kcds_launcher PUBLIC shell32 ole32 advapi32)
endif()

# Native crash capture (KCDS_ENABLE_SENTRY, Windows only): the pinned SDK, the
# kcds::launcher_sentry backend and kcds_ship_crash_handler(). See
# crash/sentry_native.cmake.
set(KCDS_LAUNCHER_SENTRY OFF)
if(WIN32 AND KCDS_ENABLE_SENTRY)
  include(crash/sentry_native.cmake)
  set(KCDS_LAUNCHER_SENTRY ON)
else()
  message(STATUS "KCDS crash capture: not built (KCDS_ENABLE_SENTRY=${KCDS_ENABLE_SENTRY})")
endif()

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME launcher.crash TARGET test_launcher_crash
                 SOURCES tests/crash_test.cpp LIBS kcds::launcher)
  if(WIN32)
    kcd2s_add_test(NAME launcher.crash_logs TARGET test_launcher_crash_logs
                   SOURCES tests/crash_logs_test.cpp LIBS kcds::launcher)
  endif()
  if(KCDS_LAUNCHER_SENTRY)
    # A controlled crash of a tiny test program (never the game) is captured
    # by the real SDK and handler and uploaded to a receiver on 127.0.0.1.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(test_launcher_crash_capture_target tests/crash_capture_target.cpp)
    target_link_libraries(test_launcher_crash_capture_target PRIVATE kcds::launcher_sentry)
    kcds_ship_crash_handler(test_launcher_crash_capture_target)
    add_test(NAME launcher.crash_capture
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/crash_capture_test.py"
              "$<TARGET_FILE:test_launcher_crash_capture_target>")
    set_tests_properties(launcher.crash_capture PROPERTIES TIMEOUT 120)
  endif()
endif()

# The launcher (private repository KCD-Synced/KCDS-Launcher), when it is checked out.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/private/CMakeLists.txt")
  add_subdirectory(private)
else()
  message(STATUS "KCDS launcher: not in this checkout (Client/launcher/private); not built")
endif()
