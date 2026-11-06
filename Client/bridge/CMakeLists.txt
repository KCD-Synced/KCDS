# Client/bridge/ — the ONLY RE-bound part.
#
# The portable core (signature scanning + resolver) builds and tests headless everywhere. The
# Windows/D3D/CryEngine glue (engine_win.*) is compiled only for a real client build on Windows;
# it needs the Win32 + DXGI headers and a MinHook dependency and is injected into WHGame.dll, so it
# cannot be built or run in the headless CI image (a).
add_library(kcd2s_bridge STATIC sigscan.cpp signature_cache.cpp apply.cpp)
add_library(kcd2s::bridge ALIAS kcd2s_bridge)
target_include_directories(kcd2s_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcd2s_bridge PUBLIC kcd2s::core kcd2s::replication kcd2s::flags)

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME bridge.sigscan TARGET test_bridge_sigscan
                 SOURCES tests/sigscan_test.cpp LIBS kcd2s::bridge)
  kcd2s_add_test(NAME bridge.signature_cache TARGET test_bridge_signature_cache
                 SOURCES tests/signature_cache_test.cpp LIBS kcd2s::bridge)
  kcd2s_add_test(NAME bridge.resolver TARGET test_bridge_resolver
                 SOURCES tests/resolver_test.cpp LIBS kcd2s::bridge)
  kcd2s_add_test(NAME bridge.apply TARGET test_bridge_apply
                 SOURCES tests/apply_test.cpp LIBS kcd2s::bridge)
endif()

# Windows engine glue: real code, only in a Windows client build. Requires MinHook (fetched/vendored
# by that build) — intentionally NOT wired into the headless preset.
if(KCD2S_BUILD_CLIENT AND WIN32)
  add_library(kcd2s_bridge_win STATIC engine_win.cpp)
  # MinHook is the client build's own copy (Client/runtime, KCD2S_MINHOOK_DIR); CMake resolves the
  # target name when it generates, whichever directory it reads first.
  target_link_libraries(kcd2s_bridge_win PUBLIC kcd2s::bridge kcd2s::replication
                        kcd2s_runtime_minhook d3d11 dxgi)
  message(STATUS "KCD2-Synced: building Windows engine bridge glue (engine_win)")

  # Diagnostic smoke-test DLL — opt-in, not part of the client. First live validation stage of the
  # resolved engine surface ("RESOLVED").
  option(KCD2S_BUILD_SMOKETEST "Build the bridge smoke-test DLL (diagnostic, injected into KCD2)" OFF)
  if(KCD2S_BUILD_SMOKETEST)
    add_subdirectory(smoketest)
  endif()
endif()
