#include "log.hpp"

#include <share.h>

#include <cstdarg>

namespace kcds::game_client {
HMODULE module;
FILE* log_file;
void log(const char* format, ...) {
  if (!log_file)
    return;
  // One formatted write per line keeps lines from different threads whole. The system tick
  // prefix lets two clients on one machine be compared line by line.
  char line[2048];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  fprintf(log_file, "[%llu] %s\n", static_cast<unsigned long long>(GetTickCount64()), line);
  fflush(log_file);
}
std::wstring module_directory() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(module, path, MAX_PATH);
  std::wstring dir(path);
  dir.resize(dir.find_last_of(L"\\") + 1);
  return dir;
}
// Opened from DllMain, before the game's main thread runs, so the branding hooks installed on
// that thread can log from the start.
void open_log() {
  std::wstring logpath =
      module_directory() + L"client-" + std::to_wstring(GetCurrentProcessId()) + L".log";
  log_file = _wfsopen(logpath.c_str(), L"w", _SH_DENYNO);
  log("KCD2-Synced native development adapter pid=%lu", GetCurrentProcessId());
}
}  // namespace kcds::game_client
