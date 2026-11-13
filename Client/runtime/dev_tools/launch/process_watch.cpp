#include "process_watch.hpp"

#include <TlHelp32.h>

#include <cwchar>
#include <iterator>

namespace kcds::runtime::launch {
namespace {

std::wstring file_name(const std::wstring& path) {
  const auto slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Whether the process runs the exe at image_path (the same install, not another copy).
bool runs_image(DWORD pid, const std::wstring& image_path) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return false;
  }
  wchar_t path[MAX_PATH * 4]{};
  DWORD size = static_cast<DWORD>(std::size(path));
  const bool named = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
  CloseHandle(process);
  return named && _wcsicmp(std::wstring(path, size).c_str(), image_path.c_str()) == 0;
}

}  // namespace

std::set<DWORD> running_processes(const wchar_t* image_name) {
  std::set<DWORD> pids;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return pids;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Process32FirstW(snapshot, &entry); more;
       more = Process32NextW(snapshot, &entry)) {
    if (_wcsicmp(entry.szExeFile, image_name) == 0) {
      pids.insert(entry.th32ProcessID);
    }
  }
  CloseHandle(snapshot);
  return pids;
}

DWORD wait_for_new_process(const std::set<DWORD>& before, const std::wstring& image_path,
                           DWORD timeout_ms) {
  const std::wstring name = file_name(image_path);
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  while (GetTickCount64() < deadline) {
    for (const DWORD pid : running_processes(name.c_str())) {
      if (before.count(pid) == 0 && runs_image(pid, image_path)) {
        return pid;
      }
    }
    Sleep(1);
  }
  return 0;
}

std::wstring full_path(const std::wstring& path) {
  wchar_t buffer[MAX_PATH * 4]{};
  const DWORD length =
      GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
  if (length == 0 || length >= std::size(buffer)) {
    return path;
  }
  return std::wstring(buffer, length);
}

}  // namespace kcds::runtime::launch
