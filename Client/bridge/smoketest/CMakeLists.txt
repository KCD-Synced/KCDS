# Bridge SMOKE TEST DLL — diagnostic only, NOT shipped. Validates the signature-resolved WHGame.dll
# surface inside the running game (see smoke_main.cpp). Windows client build only, opt-in.
#
# Build:  cmake --preset <win-client-preset> -DKCD2S_BUILD_CLIENT=ON -DKCD2S_BUILD_SMOKETEST=ON
# Output: kcd2s_smoketest.dll  — inject into a running KCD2; reads %LOCALAPPDATA%\KCD2-Synced\smoketest.log
#
# Deliberately minimal: it compiles the portable signature scanner directly (../sigscan.cpp) and the
# header-only resolver, so it does NOT drag in Backend/replication/core — only MinHook + d3d11 + dxgi.

include(FetchContent)
FetchContent_Declare(
  minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.3)              # pinned; MinHook is the same hook lib engine_win.cpp uses
FetchContent_MakeAvailable(minhook)

add_library(kcd2s_bridge_smoketest SHARED
  smoke_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../sigscan.cpp)

set_target_properties(kcd2s_bridge_smoketest PROPERTIES
  OUTPUT_NAME "kcd2s_smoketest"
  PREFIX "")

target_include_directories(kcd2s_bridge_smoketest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_link_libraries(kcd2s_bridge_smoketest PRIVATE minhook d3d11 dxgi psapi)

message(STATUS "KCD2-Synced: building bridge smoke-test DLL (kcd2s_smoketest)")
