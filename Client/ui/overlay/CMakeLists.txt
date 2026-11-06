# Custom-menu OVERLAY DLL — windowed-mode MVP of the custom front-end menu (overlay_main.cpp).
# NOT shipped: the shipped path composites CEF into the swapchain. Windows client build only, opt-in.
#
# Build:  cmake --preset <win-client-preset> -DKCD2S_BUILD_CLIENT=ON -DKCD2S_BUILD_MENU_OVERLAY=ON
# Output: kcd2s_menu_overlay.dll — inject into a running (windowed/borderless) KCD2; the menu
#         AUTO-SHOWS over the native main menu. F9 toggles it. Log: %LOCALAPPDATA%\KCD2-Synced\overlay.log
#
# Deps: the Microsoft Edge WebView2 SDK (fetched from NuGet below) + the WebView2 runtime (ships with
# Windows 10/11). The WRL headers (wrl.h) come from the Windows SDK. Bump KCD2S_WEBVIEW2_VERSION to
# move the SDK pin.

include(FetchContent)

set(KCD2S_WEBVIEW2_VERSION "1.0.2365.46" CACHE STRING "Microsoft.Web.WebView2 NuGet version to fetch")
FetchContent_Declare(
  webview2
  URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${KCD2S_WEBVIEW2_VERSION}")
FetchContent_MakeAvailable(webview2)

set(WV2_INCLUDE "${webview2_SOURCE_DIR}/build/native/include")
set(WV2_LIB     "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib")

add_library(kcd2s_menu_overlay SHARED overlay_main.cpp)
set_target_properties(kcd2s_menu_overlay PROPERTIES
  OUTPUT_NAME "kcd2s_menu_overlay"
  PREFIX ""
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)

target_include_directories(kcd2s_menu_overlay PRIVATE "${WV2_INCLUDE}")
# WebView2LoaderStatic pulls in COM + shell helpers; CreateStreamOnHGlobal needs ole32.
target_link_libraries(kcd2s_menu_overlay PRIVATE
  "${WV2_LIB}" ole32 oleaut32 shlwapi version advapi32 shell32 user32)

message(STATUS "KCD2-Synced: building custom-menu overlay DLL (kcd2s_menu_overlay, WebView2 ${KCD2S_WEBVIEW2_VERSION})")
