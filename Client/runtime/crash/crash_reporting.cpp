// Game-client crash reporting start-up; see crash_reporting.hpp.
#include "crash_reporting.hpp"

#include "crash_context.hpp"

#include <filesystem>

#include "kcds/launcher/crash.hpp"
#include "kcds/launcher/crash_handoff.hpp"
#include "kcds/launcher/crash_test.hpp"

#ifdef KCDS_ENABLE_SENTRY
// Winsock before Windows.h (core/log.hpp).
#include "../core/log.hpp"

#include <io.h>

#include <cstdio>
#include "MinHook.h"
#include "kcds/launcher/crash_logs.hpp"
#include "kcds/launcher/sentry_backend.hpp"
#endif

#ifndef KCDS_GAME_CLIENT_VERSION
#define KCDS_GAME_CLIENT_VERSION "0.0.0"
#endif

namespace kcds::runtime::crash {

namespace launcher_crash = kcds::launcher::crash;

namespace {

// KCDS_CRASH_TEST=client (verification only, launcher/crash_test.hpp): crash
// this game process on a thread of its own after the configured delay, so
// the report also shows whether the engine replaced Crashpad's handler while
// it started. Returns the text appended to the client log line.
std::string arm_crash_test() {
  if (launcher_crash::crash_test_from_environment() != launcher_crash::CrashTest::Client) {
    return std::string();
  }
  std::uint32_t seconds = launcher_crash::crash_test_delay_from_environment();
  if (!launcher_crash::schedule_crash_for_test(seconds)) {
    return "; KCDS_CRASH_TEST=client could not start its thread";
  }
  return "; KCDS_CRASH_TEST=client: the game crashes on purpose in " + std::to_string(seconds) +
         " s";
}

#ifdef KCDS_ENABLE_SENTRY
// Crashpad's filter, and the filters others asked for after it (the game's), never installed.
LPTOP_LEVEL_EXCEPTION_FILTER crashpad_filter = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER refused_filter = nullptr;
using SetFilter = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER filter);
SetFilter original_set_filter = nullptr;

// One line straight into the client log's file (its handle, no CRT lock: the crashed thread may
// hold one), flushed to disk: the log's tail goes with the crash report.
void crash_line(const char* text) {
  if (!kcds::game_client::log_file) {
    return;
  }
  const auto handle =
      reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(kcds::game_client::log_file)));
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }
  char line[512];
  const int length = std::snprintf(line, sizeof(line), "[%llu] %s\n",
                                   static_cast<unsigned long long>(GetTickCount64()), text);
  if (length <= 0) {
    return;
  }
  DWORD written = 0;
  WriteFile(handle, line, DWORD(length < int(sizeof(line)) ? length : int(sizeof(line)) - 1),
            &written, nullptr);
  FlushFileBuffers(handle);
}

// "module+0xoffset" of a code address, or the bare address outside every module.
void describe(DWORD64 address, char* out, std::size_t size) {
  HMODULE module = nullptr;
  wchar_t path[MAX_PATH] = {};
  const DWORD flags =
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
  if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(address), &module) && module &&
      GetModuleFileNameW(module, path, MAX_PATH)) {
    const wchar_t* name = wcsrchr(path, L'\\');
    std::snprintf(out, size, "%ls+0x%llx", name ? name + 1 : path,
                  static_cast<unsigned long long>(address - DWORD64(module)));
    return;
  }
  std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(address));
}

// Before Crashpad writes its dump: the exception and the crashed thread's stack, unwound from the
// exception's context, go into the client log, which reaches the crash bridge. GlitchTip shows only
// the top frame of a minidump (a crash at KERNELBASE.dll with no more).
LONG WINAPI log_then_crashpad(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord && info->ContextRecord) {
    char where[160];
    describe(DWORD64(info->ExceptionRecord->ExceptionAddress), where, sizeof(where));
    char line[512];
    std::snprintf(line, sizeof(line), "CRASH exception 0x%08lx at %s, flags 0x%lx",
                  static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode), where,
                  static_cast<unsigned long>(info->ExceptionRecord->ExceptionFlags));
    crash_line(line);
    CONTEXT context = *info->ContextRecord;
    constexpr int kFrames = 32;
    for (int depth = 0; depth < kFrames && context.Rip; ++depth) {
      describe(context.Rip, where, sizeof(where));
      std::snprintf(line, sizeof(line), "CRASH   #%02d %s", depth, where);
      crash_line(line);
      DWORD64 image = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
      if (!function) {
        // A leaf function: its return address is on top of the stack.
        if (IsBadReadPtr(reinterpret_cast<const void*>(context.Rsp), sizeof(DWORD64))) {
          break;
        }
        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
        context.Rsp += sizeof(DWORD64);
        continue;
      }
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data,
                       &establisher, nullptr);
    }
  }
  return crashpad_filter ? crashpad_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI keep_crashpad_filter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
  const LPTOP_LEVEL_EXCEPTION_FILTER before = refused_filter ? refused_filter : crashpad_filter;
  refused_filter = filter;
  return before;
}

// Keeps Crashpad's top-level filter installed from now on; false when it could not.
bool keep_filter() {
  crashpad_filter = SetUnhandledExceptionFilter(nullptr);
  if (!crashpad_filter) {
    return false;
  }
  // Ours in front of Crashpad's: it logs the crash, then hands it on.
  SetUnhandledExceptionFilter(log_then_crashpad);
  HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
  void* target =
      kernel ? reinterpret_cast<void*>(GetProcAddress(kernel, "SetUnhandledExceptionFilter"))
             : nullptr;
  const MH_STATUS ready = MH_Initialize();
  return target && (ready == MH_OK || ready == MH_ERROR_ALREADY_INITIALIZED) &&
         MH_CreateHook(target, reinterpret_cast<void*>(keep_crashpad_filter),
                       reinterpret_cast<void**>(&original_set_filter)) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}
#endif

}  // namespace

std::string start_crash_reporting(const std::wstring& module_directory,
                                  const std::wstring& client_log) {
  launcher_crash::Handoff handoff = launcher_crash::read_handoff_from_environment();
  if (!handoff.settings.enabled) {
    return "Crash reporting off: the launcher handed over no DSN";
  }

  launcher_crash::RuntimeInfo info;
  info.release = std::string("kcds-game-client@") + KCDS_GAME_CLIENT_VERSION;

  launcher_crash::Backend* backend = nullptr;
#ifdef KCDS_ENABLE_SENTRY
  std::filesystem::path database = launcher_crash::default_database_dir("game-client");
  std::u8string database_utf8 = database.u8string();
  info.database_dir.assign(database_utf8.begin(), database_utf8.end());

  launcher_crash::SentryOptions options;
  options.handler = std::filesystem::path(module_directory) / L"crashpad_handler.exe";
  // The tail of this process's client log (in the logs folder) goes with a crash report.
  options.logs.push_back(launcher_crash::game_client_log(
      std::filesystem::path(client_log).parent_path(), GetCurrentProcessId(), database));
  // The stage, the thread, the last script command and the stack go with a report.
  options.annotate = annotate;
  // Intentionally never deleted: destroying it would shut the SDK down at
  // process exit, under loader lock.
  backend = new launcher_crash::SentryBackend(options);
#else
  (void)module_directory;
  (void)client_log;
#endif

  launcher_crash::State state =
      launcher_crash::start(handoff.settings, handoff.allow_http_loopback, info, backend);
  if (state != launcher_crash::State::Started) {
    // Nothing was initialised, so releasing the unused backend is safe.
    delete backend;
  }
  std::string line = "Crash reporting: ";
  line += launcher_crash::describe(state);
  line += " (release " + info.release + ", environment " + handoff.settings.environment + ")";
  if (state == launcher_crash::State::Started) {
#ifdef KCDS_ENABLE_SENTRY
    line += keep_filter() ? "; its exception filter is kept"
                          : "; its exception filter could not be kept (the game may replace it)";
#endif
    line += arm_crash_test();
  }
  return line;
}

}  // namespace kcds::runtime::crash
