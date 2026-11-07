cmake_minimum_required(VERSION 3.20)

project(kcd2_synced
  VERSION 1.0.1
  DESCRIPTION "KCD2-Synced — multiplayer mod + SDK for Kingdom Come: Deliverance 2"
  LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(KCD2S_BUILD_HEADLESS "Build headless libs: Backend/core/wire/transport/replication + tests" ON)
option(KCD2S_BUILD_SERVER   "Build the authoritative server binary"                        ON)
option(KCD2S_BUILD_CLIENT   "Build the WHGame engine bridge/injector (Windows x64 only)"   ${WIN32})
option(KCD2S_BUILD_TESTS    "Build the test suite"                                         ON)
option(KCD2S_ENABLE_ASAN    "Build with AddressSanitizer + UBSan"                          OFF)
option(KCD2S_ENABLE_TSAN    "Build with ThreadSanitizer"                                   OFF)
# Native crash capture (pinned sentry-native with the Crashpad backend) for the
# launcher and the game-client DLL. Windows only; fetched at configure time.
# Nothing is sent unless a deployment configuration provides our own DSN.
option(KCDS_ENABLE_SENTRY   "Link native crash capture into the launcher and game client"  ${WIN32})

# ---------------------------------------------------------------------------
# Global C++ settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Release keeps full optimization and also writes PDBs for every target, third-party ones included:
# private debug symbols for the crash server. They are never shipped. /DEBUG turns /OPT:REF and /OPT:ICF
# off, so they are set again; /FS lets parallel compiles share a PDB. C and C++ only: MASM sources
# (crashpad's) take neither flag from llvm-ml.
if(MSVC)
  add_compile_options($<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Zi>
    $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/FS>)
  add_link_options($<$<CONFIG:Release>:/DEBUG:FULL> $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# Warnings-as-errors (CI gate). Keep flags on an INTERFACE target every module links.
add_library(kcd2s_flags INTERFACE)
add_library(kcd2s::flags ALIAS kcd2s_flags)
if(MSVC)
  target_compile_options(kcd2s_flags INTERFACE /W4 /permissive- /EHsc)
else()
  target_compile_options(kcd2s_flags INTERFACE -Wall -Wextra -Wpedantic)
endif()

if(KCD2S_ENABLE_ASAN AND NOT MSVC)
  target_compile_options(kcd2s_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(kcd2s_flags INTERFACE -fsanitize=address,undefined)
endif()
if(KCD2S_ENABLE_TSAN AND NOT MSVC)
  target_compile_options(kcd2s_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(kcd2s_flags INTERFACE -fsanitize=thread)
endif()

# The handshake key. Only a client and a server built with the same key connect: the
# KSTP CHALLENGE_RESPONSE carries an HMAC under it, so nothing on the wire can be replayed.
#
# - KCD2S_BUILD_SALT is the wire label. It carries the version, so two releases refuse each other at
#   the handshake instead of misreading each other's state. A client-only hotfix pins the previous
#   release's label: -DKCD2S_BUILD_SALT=kcds-<previous version>.
# - The build secret (KCDS_BUILD_SECRET_FILE, or the KCDS_BUILD_SECRET environment variable) is the
#   private part: a release build without it is refused (KCDS_RELEASE_BUILD). It is read into a
#   normal variable, never the cache.
# - key = SHA-256("kcds-handshake-v1:" secret ":" label), derived here. Only the key's bytes are
#   compiled in (core/version.hpp); the secret and the label never reach a binary, and SHA-256 does
#   not run backwards. A key in a shipped binary can still be dug out by reverse engineering it;
#   the licence gate stays the real guard against bots.
set(KCD2S_BUILD_SALT "kcds-${PROJECT_VERSION}" CACHE STRING
    "Wire label of the build (a client-only hotfix pins the previous release's)")
option(KCDS_RELEASE_BUILD "A published build: the build secret is required" OFF)
set(KCDS_BUILD_SECRET_FILE "" CACHE FILEPATH "File holding the release build secret")
set(kcds_build_secret "")
if(KCDS_BUILD_SECRET_FILE)
  file(READ "${KCDS_BUILD_SECRET_FILE}" kcds_build_secret)
  string(STRIP "${kcds_build_secret}" kcds_build_secret)
elseif(DEFINED ENV{KCDS_BUILD_SECRET})
  set(kcds_build_secret "$ENV{KCDS_BUILD_SECRET}")
endif()
string(LENGTH "${kcds_build_secret}" kcds_build_secret_length)
if(KCDS_RELEASE_BUILD AND kcds_build_secret_length LESS 32)
  message(FATAL_ERROR "KCDS_RELEASE_BUILD needs the build secret (32 characters or more) in "
                      "KCDS_BUILD_SECRET_FILE or the KCDS_BUILD_SECRET environment variable")
endif()
if(kcds_build_secret_length EQUAL 0)
  message(STATUS "KCDS handshake key: development (no build secret)")
else()
  message(STATUS "KCDS handshake key: from the build secret")
endif()
string(SHA256 kcds_handshake_key "kcds-handshake-v1:${kcds_build_secret}:${KCD2S_BUILD_SALT}")
# The in-game bug reports' key: SHA-256("kcds-bug-report-v1:" secret), the same for every version.
# The client signs its reports with it and the KCDS Discord bot checks them with the same key; the
# secret never reaches a binary.
string(SHA256 kcds_bug_report_key "kcds-bug-report-v1:${kcds_build_secret}")
set(kcds_build_secret "")
set(kcds_bug_key_bytes "")
foreach(kcds_byte_index RANGE 0 62 2)
  string(SUBSTRING "${kcds_bug_report_key}" ${kcds_byte_index} 2 kcds_byte)
  list(APPEND kcds_bug_key_bytes "0x${kcds_byte}")
endforeach()
list(JOIN kcds_bug_key_bytes "," kcds_bug_key_list)
target_compile_definitions(kcd2s_flags INTERFACE "KCDS_BUG_REPORT_KEY=${kcds_bug_key_list}")
string(SHA256 kcds_fingerprint_hash "kcds-fingerprint-v1:${kcds_handshake_key}")
string(SUBSTRING "${kcds_fingerprint_hash}" 0 8 kcds_fingerprint_hex)
set(kcds_key_bytes "")
foreach(kcds_byte_index RANGE 0 62 2)
  string(SUBSTRING "${kcds_handshake_key}" ${kcds_byte_index} 2 kcds_byte)
  list(APPEND kcds_key_bytes "0x${kcds_byte}")
endforeach()
list(JOIN kcds_key_bytes "," kcds_key_list)
target_compile_definitions(kcd2s_flags INTERFACE "KCDS_HANDSHAKE_KEY=${kcds_key_list}"
                                                 "KCDS_BUILD_FINGERPRINT=0x${kcds_fingerprint_hex}u")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(KCD2S_BUILD_TESTS)
  enable_testing()
  add_subdirectory(Dev/testkit)   # in-house header-only test harness
endif()

# ---------------------------------------------------------------------------
# Headless libraries (platform-agnostic; built on Linux CI)
# ---------------------------------------------------------------------------
if(KCD2S_BUILD_HEADLESS)
  add_subdirectory(Backend/core)
  add_subdirectory(Backend/wire)
  add_subdirectory(Backend/transport)
  add_subdirectory(Backend/replication)
  add_subdirectory(Backend/voice)
  add_subdirectory(Client/bridge)  # portable sigscan/resolver core (Windows glue gated)
  add_subdirectory(Backend/integration)
  # Before Client/runtime: the game client links the launcher's crash capture.
  add_subdirectory(Client/launcher)  # KCDS launcher updater core, headless-testable
  # Before Client/runtime: the game client links the resource web views on CEF
  # and ships their runtime with kcds_ship_cef().
  option(KCD2S_BUILD_GAME_CLIENT "Build native experimental multiplayer adapter" OFF)
  if(WIN32 AND KCD2S_BUILD_CLIENT AND KCD2S_BUILD_GAME_CLIENT)
    add_subdirectory(Client/ui/web)
  endif()
  add_subdirectory(Client/voice)
  add_subdirectory(Client/runtime)
  add_subdirectory(Backend/sdk)
  add_subdirectory(Backend/scripting)
  add_subdirectory(Client/masterclient)
endif()

# if(KCD2S_BUILD_SERVER)
#   add_subdirectory(Backend/integration/server)   # build-order step 6
# endif()

if(KCD2S_BUILD_CLIENT)
  if(NOT WIN32)
    message(WARNING "KCD2S_BUILD_CLIENT is ON but the engine bridge targets Windows x64; skipping.")
  else()
    # add_subdirectory(Client)  # build-order step 7

    # Custom-menu overlay DLL (WebView2) — windowed-mode MVP of the custom front-end menu. Opt-in,
    # not part of the shipped client (which composites CEF into the swapchain).
    option(KCD2S_BUILD_MENU_OVERLAY "Build the WebView2 custom-menu overlay DLL (injected into KCD2)" OFF)
    if(KCD2S_BUILD_MENU_OVERLAY)
      add_subdirectory(Client/ui/overlay)
    endif()

    # The in-frame menu compositor and the native main-menu hooks are linked into the game
    # client DLL (Client/runtime) and built with it.
    if(KCD2S_BUILD_GAME_CLIENT)
      add_subdirectory(Client/menu_native)
      add_subdirectory(Client/ui/compositor)
    endif()
  endif()
endif()

message(STATUS "KCD2-Synced: headless=${KCD2S_BUILD_HEADLESS} server=${KCD2S_BUILD_SERVER} "
               "client=${KCD2S_BUILD_CLIENT} tests=${KCD2S_BUILD_TESTS} "
               "asan=${KCD2S_ENABLE_ASAN} tsan=${KCD2S_ENABLE_TSAN}")
