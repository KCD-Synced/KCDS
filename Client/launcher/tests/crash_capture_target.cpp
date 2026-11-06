// Test program for launcher.crash_capture (driven by crash_capture_test.py).
//
//   test_launcher_crash_capture_target <dsn> <database-dir> <environment>
//
// Starts crash reporting exactly as the launcher does (crash::start with the
// SentryBackend and crashpad_handler.exe next to this executable), writes a
// log longer than the attachment limit (<database-dir>.log, first line
// KCDS-CAPTURE-LOG-HEAD, last line KCDS-CAPTURE-LOG-TAIL) and attaches it like
// the game client attaches its log, then crashes itself with a null-pointer
// write. Plain HTTP is allowed only for a
// loopback DSN, so the report can only reach the test's local receiver.
//
// Exit codes when no crash happens: 3 = reporting disabled (empty DSN),
// 4 = any other start result. A crash ends the process with the access
// violation status.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "kcds/launcher/crash.hpp"
#include "kcds/launcher/sentry_backend.hpp"

namespace crash = kcds::launcher::crash;

namespace {

std::filesystem::path executable_directory() {
  std::vector<wchar_t> buffer(32768);
  DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return std::filesystem::path();
  }
  return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

// Kept out of line so the faulting frame is our own function.
__declspec(noinline) void crash_now() {
  volatile int* target = nullptr;
  *target = 42;
}

// Writes the head line, enough filler to exceed the attachment limit, and the
// tail line, so the report must carry a truncated tail of the log.
void write_capture_log(const std::filesystem::path& path) {
  std::ofstream log(path, std::ios::binary | std::ios::trunc);
  log << "KCDS-CAPTURE-LOG-HEAD\n";
  std::string filler(99, '.');
  filler += '\n';
  std::uint64_t written = 0;
  while (written < crash::kMaxLogAttachmentBytes + 64 * 1024) {
    log << filler;
    written += filler.size();
  }
  log << "KCDS-CAPTURE-LOG-TAIL\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: test_launcher_crash_capture_target <dsn> <database> <env>\n");
    return 2;
  }
  // No error dialog may block the test after the handler took the report.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

  crash::Settings settings;
  settings.dsn = argv[1];
  settings.enabled = !settings.dsn.empty();
  settings.environment = argv[3];

  crash::RuntimeInfo info;
  info.release = "kcds-crash-capture@0.0.1";
  info.database_dir = argv[2];

  crash::SentryOptions options;
  options.handler = executable_directory() / L"crashpad_handler.exe";
  options.wait_for_upload = true;
  std::filesystem::path database(argv[2]);
  crash::LogAttachment capture_log;
  capture_log.source = std::filesystem::path(database.native() + L".log");
  capture_log.snapshot = database / L"logs" / L"capture.log";
  options.logs.push_back(capture_log);
  crash::SentryBackend backend(options);

  crash::State state = crash::start(settings, true, info, &backend);
  std::printf("%s\n", crash::describe(state));
  std::fflush(stdout);
  if (state == crash::State::Disabled) {
    return 3;
  }
  if (state != crash::State::Started) {
    return 4;
  }
  write_capture_log(capture_log.source);
  crash_now();
  return 5;
}
