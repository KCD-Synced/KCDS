# Chromium Embedded Framework for resource web views.
#
# The official CEF "minimal" binary distribution for Windows x64 (BSD licence, from the CEF
# project's build server) is fetched at a pinned version and verified by SHA-1, the hash the
# build index publishes. Only CEF's own binaries are used; no other framework's copy.
# For an offline build, extract the same archive and pass
# -DFETCHCONTENT_SOURCE_DIR_KCDS_CEF=<directory>.
#
# Build choices:
#   - the C++ wrapper (libcef_dll_wrapper) is built from the distribution's sources with the
#     dynamic CRT (/MD) like the rest of the game client;
#   - no sandbox (it needs a static-CRT executable and a separate bootstrap);
#   - kcds_ship_cef(target) copies the runtime (libcef.dll, resources, locales) next to a target.
include(FetchContent)

set(KCDS_CEF_VERSION "154.0.28+g564dd6c+chromium-154.0.8037.58")
set(KCDS_CEF_SHA1 "34a32277db8addfa355a009ed16c8dae939daed1")
set(KCDS_CEF_ARCHIVE "cef_binary_${KCDS_CEF_VERSION}_windows64_minimal.tar.bz2")
string(REPLACE "+" "%2B" KCDS_CEF_ARCHIVE_URL_NAME "${KCDS_CEF_ARCHIVE}")
set(KCDS_CEF_URL "https://cef-builds.spotifycdn.com/${KCDS_CEF_ARCHIVE_URL_NAME}")
# A build without downloads (KCDS_DEPS_DIR, a folder of the pinned archives): the same archive,
# checked against the same SHA-1.
set(KCDS_DEPS_DIR "" CACHE PATH "Folder of the pinned dependency archives, used instead of downloading them")
if(KCDS_DEPS_DIR AND EXISTS "${KCDS_DEPS_DIR}/${KCDS_CEF_ARCHIVE}")
  set(KCDS_CEF_URL "${KCDS_DEPS_DIR}/${KCDS_CEF_ARCHIVE}")
endif()

# SOURCE_SUBDIR names a folder the archive does not have, so CEF's own sample projects are not
# added; only the wrapper below is built.
FetchContent_Declare(kcds_cef
  URL "${KCDS_CEF_URL}"
  URL_HASH "SHA1=${KCDS_CEF_SHA1}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR kcds-no-cmake-project)
FetchContent_MakeAvailable(kcds_cef)

set(CEF_ROOT "${kcds_cef_SOURCE_DIR}")
set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "KCDS links CEF's wrapper with the dynamic CRT" FORCE)
set(USE_SANDBOX OFF CACHE BOOL "KCDS runs CEF without the sandbox" FORCE)
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
# clang-cl (the Linux cross build) warns about CEF's own wrapper code where MSVC does
# not; CEF builds it with warnings as errors (/WX). Warnings stay warnings there.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  list(REMOVE_ITEM CEF_COMPILER_FLAGS /WX)
endif()
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_BINARY_DIR}/libcef_dll_wrapper")
message(STATUS "KCDS web views: CEF ${KCDS_CEF_VERSION}")

# FindCEF's variables live in this directory only; kcds_ship_cef() runs in the game client's.
if(NOT IS_DIRECTORY "${CEF_BINARY_DIR_RELEASE}" OR NOT IS_DIRECTORY "${CEF_RESOURCE_DIR}")
  message(FATAL_ERROR "CEF distribution incomplete: ${CEF_BINARY_DIR_RELEASE} ${CEF_RESOURCE_DIR}")
endif()
set(KCDS_CEF_BINARIES "${CEF_BINARY_DIR_RELEASE}" CACHE INTERNAL "CEF release binaries")
set(KCDS_CEF_RESOURCES "${CEF_RESOURCE_DIR}" CACHE INTERNAL "CEF resources and locales")

# The include root, libcef.lib and the wrapper, for targets that use CEF.
add_library(kcds_cef INTERFACE)
add_library(kcds::cef ALIAS kcds_cef)
target_include_directories(kcds_cef SYSTEM INTERFACE "${CEF_ROOT}")
target_compile_options(kcds_cef INTERFACE /external:W0)
target_link_libraries(kcds_cef INTERFACE libcef_dll_wrapper "${KCDS_CEF_BINARIES}/libcef.lib")

# Copies the CEF runtime next to target's output after each build: the release zip ships the
# same files next to the game-client DLL.
function(kcds_ship_cef target)
  if(NOT IS_DIRECTORY "${KCDS_CEF_BINARIES}" OR NOT IS_DIRECTORY "${KCDS_CEF_RESOURCES}")
    message(FATAL_ERROR "kcds_ship_cef: the CEF folders are not known")
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KCDS_CEF_BINARIES}"
            "$<TARGET_FILE_DIR:${target}>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KCDS_CEF_RESOURCES}"
            "$<TARGET_FILE_DIR:${target}>"
    VERBATIM)
endfunction()
