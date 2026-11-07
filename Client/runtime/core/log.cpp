#include "log.hpp"

#include <share.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdarg>
#include <filesystem>
#include <system_error>
#include <vector>

namespace kcds::game_client {
HMODULE module;
FILE* log_file;
namespace {
// Client logs kept in the logs folder: the oldest beyond these go when a game starts.
constexpr std::size_t kKeptClientLogs = 20;
std::wstring opened_log_path;

// Deletes the oldest client-<pid>.log files of `folder` beyond kKeptClientLogs.
void prune_client_logs(const std::filesystem::path& folder) {
  std::error_code error;
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    const std::wstring name = entry.path().filename().wstring();
    if (name.rfind(L"client-", 0) == 0 && entry.path().extension() == L".log") {
      found.emplace_back(entry.last_write_time(error), entry.path());
    }
  }
  if (found.size() <= kKeptClientLogs) {
    return;
  }
  std::sort(found.begin(), found.end());
  for (std::size_t index = 0; index + kKeptClientLogs < found.size(); ++index) {
    std::filesystem::remove(found[index].second, error);
  }
}
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
    prune_client_logs(folder);
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
