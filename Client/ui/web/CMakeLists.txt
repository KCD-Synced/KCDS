# Resource web views, linked into the game client DLL. See
# include/kcds/web/web.hpp. CEF itself comes from cef.cmake (official binary distribution).
include(${CMAKE_CURRENT_SOURCE_DIR}/cef.cmake)

add_library(kcds_web STATIC web_host.cpp)
add_library(kcds::web ALIAS kcds_web)
target_include_directories(kcds_web
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kcds_web PRIVATE kcd2s::flags kcds::cef d3d11 dxgi d2d1)
# libcef.dll lives next to the game client DLL, not next to KingdomCome.exe: it is loaded on first
# use from the runtime folder (SetDllDirectory + delay load).
target_link_options(kcds_web INTERFACE /DELAYLOAD:libcef.dll)
target_link_libraries(kcds_web INTERFACE delayimp)

# CEF's renderer, GPU and utility processes.
add_executable(kcds-web-helper WIN32 helper/helper_main.cpp)
target_include_directories(kcds-web-helper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kcds-web-helper PRIVATE kcds::cef)
