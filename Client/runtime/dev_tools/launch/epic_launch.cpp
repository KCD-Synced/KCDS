#include "epic_launch.hpp"

#include "inject.hpp"
#include "process_watch.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cwchar>
#include <set>

namespace kcds::runtime::launch {
namespace {

std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
  if (size > 0) {
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
  }
  return out;
}

}  // namespace

int launch_epic(const EpicIds& ids, const std::wstring& game, const std::wstring& dll) {
  const std::wstring game_path = full_path(game);
  const std::set<DWORD> before = running_processes(L"KingdomCome.exe");
  const std::wstring uri = widen(epic_launch_uri(ids));
  printf("Starting the game through the Epic Games launcher (%s)\n", ids.app_name.c_str());
  const auto opened = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (opened <= 32) {
    printf("The Epic Games launcher could not be opened (ShellExecute %lld)\n",
           static_cast<long long>(opened));
    return 5;
  }
  const DWORD pid = wait_for_new_process(before, game_path, kStoreLaunchTimeoutMs);
  if (pid == 0) {
    printf("The Epic Games launcher did not start the game within %lu s\n",
           kStoreLaunchTimeoutMs / 1000);
    return 5;
  }
  HANDLE process =
      OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                      PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_TERMINATE | SYNCHRONIZE,
                  FALSE, pid);
  if (!process) {
    printf("The game started by the Epic Games launcher could not be opened (PID=%lu error=%lu)\n",
           pid, GetLastError());
    return 1;
  }
  int result = 0;
  if (!load_client(process, dll)) {
    printf("Injection failed in the game the Epic Games launcher started (PID=%lu)\n", pid);
    TerminateProcess(process, 1);
    result = 1;
  } else {
    printf("Started own client PID=%lu (Epic Games Store)\n", pid);
    if (WaitForSingleObject(process, kEarlyExitMs) == WAIT_OBJECT_0) {
      DWORD exit_code = 0;
      GetExitCodeProcess(process, &exit_code);
      printf("The game closed right after its start (exit code %lu)\n", exit_code);
      result = 4;
    }
  }
  CloseHandle(process);
  return result;
}

}  // namespace kcds::runtime::launch
