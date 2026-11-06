#include "runtime_profiles.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <system_error>
#include <vector>

namespace kcds::resources::profiles {
namespace {

constexpr wchar_t kProfiles[] = L"\\KCDS\\ResourceRuntime";
constexpr wchar_t kRuntimeImage[] = L"kcd2s-resource-runtime.exe";
// The WebView2 crash reports kept from removed profiles (beside KCDS\ResourceRuntime), the newest.
constexpr wchar_t kKeptReports[] = L"crash\\webview";
constexpr std::size_t kKeptReportCount = 5;

// The process id a profile folder is named after, or 0 for any other name.
std::uint32_t pid_of(const std::wstring& name) {
  if (name.empty() || name.size() > 10) {
    return 0;
  }
  std::uint64_t pid = 0;
  for (const wchar_t c : name) {
    if (!std::iswdigit(c)) {
      return 0;
    }
    pid = pid * 10 + std::uint64_t(c - L'0');
  }
  return pid <= 0xFFFFFFFFu ? std::uint32_t(pid) : 0;
}

bool ends_with_image(const std::wstring& path) {
  const std::wstring image = kRuntimeImage;
  if (path.size() < image.size()) {
    return false;
  }
  const std::wstring tail = path.substr(path.size() - image.size());
  return _wcsicmp(tail.c_str(), image.c_str()) == 0;
}

// Moves a profile's WebView2 crash reports (its Crashpad's reports\*.dmp) out before the profile
// is removed, so a browser process that failed can be read afterwards: to
// %LOCALAPPDATA%\KCDS\crash\webview as <profile>-<report>.dmp, the newest kKeptReportCount kept.
void keep_reports(const std::filesystem::path& profile) {
  std::error_code error;
  const std::filesystem::path reports = profile / L"EBWebView" / L"Crashpad" / L"reports";
  if (!std::filesystem::is_directory(reports, error)) {
    return;
  }
  const std::filesystem::path kept = profile.parent_path().parent_path() / kKeptReports;
  bool moved = false;
  for (const auto& entry : std::filesystem::directory_iterator(reports, error)) {
    if (entry.path().extension() != L".dmp") {
      continue;
    }
    std::filesystem::create_directories(kept, error);
    const std::wstring name =
        profile.filename().wstring() + L"-" + entry.path().filename().wstring();
    std::error_code copy_error;
    std::filesystem::copy_file(entry.path(), kept / name,
                               std::filesystem::copy_options::overwrite_existing, copy_error);
    moved = moved || !copy_error;
  }
  if (!moved) {
    return;
  }
  std::vector<std::filesystem::directory_entry> dumps;
  for (const auto& entry : std::filesystem::directory_iterator(kept, error)) {
    dumps.push_back(entry);
  }
  std::sort(dumps.begin(), dumps.end(), [](const auto& a, const auto& b) {
    std::error_code time_error;
    return a.last_write_time(time_error) > b.last_write_time(time_error);
  });
  for (std::size_t index = kKeptReportCount; index < dumps.size(); ++index) {
    std::error_code remove_error;
    std::filesystem::remove(dumps[index].path(), remove_error);
  }
}

}  // namespace

std::size_t sweep(const std::wstring& base, std::uint32_t own, const Alive& alive) {
  std::error_code error;
  std::size_t removed = 0;
  for (const auto& entry : std::filesystem::directory_iterator(base, error)) {
    if (!entry.is_directory(error)) {
      continue;
    }
    const std::uint32_t pid = pid_of(entry.path().filename().wstring());
    if (!pid || pid == own || alive(pid)) {
      continue;
    }
    keep_reports(entry.path());
    std::error_code remove_error;
    std::filesystem::remove_all(entry.path(), remove_error);
    if (!remove_error) {
      ++removed;
    }
  }
  return removed;
}

bool runtime_alive(std::uint32_t pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    // Another user's or a protected process holds the id: not ours to judge, keep its folder.
    return GetLastError() == ERROR_ACCESS_DENIED;
  }
  wchar_t path[MAX_PATH];
  DWORD size = MAX_PATH;
  bool runtime = false;
  DWORD exit_code = 0;
  if (GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE &&
      QueryFullProcessImageNameW(process, 0, path, &size)) {
    runtime = ends_with_image(std::wstring(path, size));
  }
  CloseHandle(process);
  return runtime;
}

std::wstring open(const std::wstring& local_app_data) {
  const std::wstring base = local_app_data + kProfiles;
  const std::uint32_t own = GetCurrentProcessId();
  sweep(base, own, runtime_alive);
  return base + L"\\" + std::to_wstring(own);
}

bool close(const std::wstring& folder) {
  keep_reports(folder);
  std::error_code error;
  std::filesystem::remove_all(folder, error);
  return !error;
}

}  // namespace kcds::resources::profiles
