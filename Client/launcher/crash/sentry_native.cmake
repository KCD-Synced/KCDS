# Native crash capture for the launcher and the game-client DLL.
#
# sentry-native (MIT) is fetched as its pinned release source archive, which
# bundles the Crashpad backend. The archive is verified by SHA-256 before use.
# For an offline build, extract the same archive and pass
# -DFETCHCONTENT_SOURCE_DIR_KCDS_SENTRY_NATIVE=<directory>.
#
# Build choices:
#   - static library (no sentry.dll to ship);
#   - Crashpad backend: crashpad_handler.exe is a shipped file next to the
#     launcher and the game-client DLL;
#   - WinHTTP transport (the handler also uploads through WinHTTP);
#   - no SDK tests, examples or install rules.
#
# Linking the SDK never sends anything by itself: crash::start() only
# initialises it when the deployment configuration enables reporting with our
# own DSN.
include(FetchContent)

set(KCDS_SENTRY_NATIVE_VERSION "0.14.2")
set(KCDS_SENTRY_NATIVE_SHA256 "65ebb14fa305dcef7b56347cdd5afcbac3c2d0f129c0e23b53933fae4e39c508")
set(KCDS_SENTRY_NATIVE_URL
    "https://github.com/getsentry/sentry-native/releases/download/${KCDS_SENTRY_NATIVE_VERSION}/sentry-native.zip")
# A build without downloads (KCDS_DEPS_DIR, a folder of the pinned archives): the same archive,
# checked against the same SHA-256.
set(KCDS_DEPS_DIR "" CACHE PATH "Folder of the pinned dependency archives, used instead of downloading them")
if(KCDS_DEPS_DIR AND EXISTS "${KCDS_DEPS_DIR}/sentry-native-${KCDS_SENTRY_NATIVE_VERSION}.zip")
  set(KCDS_SENTRY_NATIVE_URL "${KCDS_DEPS_DIR}/sentry-native-${KCDS_SENTRY_NATIVE_VERSION}.zip")
endif()

set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "KCDS links the crash SDK statically" FORCE)
set(SENTRY_BACKEND "crashpad" CACHE STRING "KCDS uses the Crashpad backend" FORCE)
set(SENTRY_TRANSPORT "winhttp" CACHE STRING "KCDS uses the WinHTTP transport" FORCE)
set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SENTRY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(CRASHPAD_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(kcds_sentry_native
  URL "${KCDS_SENTRY_NATIVE_URL}"
  URL_HASH "SHA256=${KCDS_SENTRY_NATIVE_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(kcds_sentry_native)
message(STATUS "KCDS crash capture: sentry-native ${KCDS_SENTRY_NATIVE_VERSION} (crashpad, winhttp)")

# The launcher's and the game client's crash-capture backend.
add_library(kcds_launcher_sentry STATIC ${CMAKE_CURRENT_LIST_DIR}/sentry_backend.cpp)
add_library(kcds::launcher_sentry ALIAS kcds_launcher_sentry)
target_link_libraries(kcds_launcher_sentry PUBLIC kcds::launcher PRIVATE sentry::sentry shell32 ole32)
target_compile_definitions(kcds_launcher_sentry PUBLIC KCDS_ENABLE_SENTRY=1)
add_dependencies(kcds_launcher_sentry crashpad_handler)

# Copies crashpad_handler.exe next to target's output after each build, so the
# launcher, the game-client DLL and the crash-capture test find the handler
# where a release installs it.
function(kcds_ship_crash_handler target)
  add_dependencies(${target} crashpad_handler)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:crashpad_handler>" "$<TARGET_FILE_DIR:${target}>"
    VERBATIM)
endfunction()
