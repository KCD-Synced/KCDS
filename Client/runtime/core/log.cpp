#include "log.hpp"

#include <share.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdint>
#include <filesystem>

#include "../logging/log_files.hpp"

namespace kcds::game_client {
HMODULE module;
FILE* log_file;
namespace {
std::wstring opened_log_path;
// Bytes written to the log since it was opened or last started over (under the file's lock).
std::uintmax_t written_bytes = 0;
}  // namespace

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
  const auto tick = static_cast<unsigned long long>(GetTickCount64());
  // The file's own lock makes the size check and a start-over one step with the write.
  _lock_file(log_file);
  const int length = fprintf(log_file, "[%llu] %s\n", tick, line);
  fflush(log_file);
  if (length > 0) {
    written_bytes += std::uintmax_t(length);
  }
  // A long session must not fill the disk: a full log moves to its earlier part and starts over.
  if (written_bytes >= log_files::kPartBytes &&
      log_files::start_over(log_file, std::filesystem::path(opened_log_path))) {
    written_bytes = 0;
    fprintf(log_file, "[%llu] log continued; the lines before are in %ls\n", tick,
            log_files::earlier_part(opened_log_path).filename().c_str());
    fflush(log_file);
  }
  _unlock_file(log_file);
}
std::wstring module_directory() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(module, path, MAX_PATH);
  std::wstring dir(path);
  dir.resize(dir.find_last_of(L"\\") + 1);
  return dir;
}
std::wstring logs_directory() {
  PWSTR local = nullptr;
  std::wstring folder;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)) &&
      local) {
    folder = std::wstring(local) + L"\\KCDS\\logs\\";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
      folder.clear();
    }
  }
  CoTaskMemFree(local);
  return folder;
}
std::wstring client_log_path() {
  return opened_log_path;
}
// Opened from DllMain, before the game's main thread runs, so the branding hooks installed on
// that thread can log from the start. It goes in the logs folder beside the launcher's, so one
// folder holds every log a player sends; beside the DLL when that folder cannot be made.
void open_log() {
  const std::wstring name = L"client-" + std::to_wstring(GetCurrentProcessId()) + L".log";
  const std::wstring folder = logs_directory();
  if (!folder.empty()) {
    log_files::prune(folder, log_files::kKeptFiles, log_files::kKeptBytes);
    opened_log_path = folder + name;
    log_file = _wfsopen(opened_log_path.c_str(), L"w", _SH_DENYNO);
  }
  if (!log_file) {
    opened_log_path = module_directory() + name;
    log_file = _wfsopen(opened_log_path.c_str(), L"w", _SH_DENYNO);
  }
  log("KCD2-Synced native development adapter pid=%lu", GetCurrentProcessId());
}
}  // namespace kcds::game_client
