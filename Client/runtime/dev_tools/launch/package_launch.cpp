#include "package_launch.hpp"

#include "inject.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <appmodel.h>
#include <shobjidl.h>

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <string>

namespace kcds::runtime::launch {
namespace {

constexpr const wchar_t* kAttachSwitch = L"--package-attach";
constexpr const wchar_t* kGameImage = L"KingdomCome.exe";
// How long a launch helper may take to start the game.
constexpr DWORD kHelperChildTimeoutMs = 60000;
// How long the starting side waits for the debugger side's result.
constexpr DWORD kResultTimeoutMs = kInjectTimeoutMs + kHelperChildTimeoutMs + 10000;
// How long it still waits when Windows reported the activation as failed.
constexpr DWORD kFailedActivationResultMs = 20000;

template <typename Interface>
struct ComPointer {
  Interface* pointer = nullptr;
  ~ComPointer() {
    if (pointer) {
      pointer->Release();
    }
  }
  Interface* operator->() const {
    return pointer;
  }
};

std::wstring file_name(const std::wstring& path) {
  const auto slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring image_path(HANDLE process) {
  wchar_t path[MAX_PATH * 4]{};
  DWORD size = static_cast<DWORD>(std::size(path));
  if (!QueryFullProcessImageNameW(process, 0, path, &size)) {
    return {};
  }
  return std::wstring(path, size);
}

bool is_game_image(HANDLE process) {
  return _wcsicmp(file_name(image_path(process)).c_str(), kGameImage) == 0;
}

bool process_alive(DWORD pid) {
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process) {
    return false;
  }
  const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return alive;
}

// A KingdomCome.exe started by parent, or 0 once timeout_ms passed.
DWORD wait_for_child_game(DWORD parent, DWORD timeout_ms) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  while (GetTickCount64() < deadline) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
      PROCESSENTRY32W entry{};
      entry.dwSize = sizeof(entry);
      for (BOOL more = Process32FirstW(snapshot, &entry); more;
           more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID == parent && _wcsicmp(entry.szExeFile, kGameImage) == 0) {
          CloseHandle(snapshot);
          return entry.th32ProcessID;
        }
      }
      CloseHandle(snapshot);
    }
    Sleep(2);
  }
  return 0;
}

HANDLE open_for_injection(DWORD pid) {
  return OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                         PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_TERMINATE | SYNCHRONIZE,
                     FALSE, pid);
}

void disable_debugging(const std::wstring& full_name) {
  ComPointer<IPackageDebugSettings> settings;
  if (SUCCEEDED(CoCreateInstance(CLSID_PackageDebugSettings, nullptr, CLSCTX_ALL,
                                 IID_PPV_ARGS(&settings.pointer)))) {
    settings->DisableDebugging(full_name.c_str());
  }
}

// The result line "started <pid>" or "failed <reason>", written whole (temporary file + rename).
void write_result(const std::wstring& path, const std::string& line) {
  const std::wstring temporary = path + L".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << line << "\n";
  }
  MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string read_result(const std::wstring& path, DWORD timeout_ms) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  while (true) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (in && std::getline(in, line) && !line.empty()) {
      in.close();
      DeleteFileW(path.c_str());
      return line;
    }
    if (GetTickCount64() >= deadline) {
      return {};
    }
    Sleep(100);
  }
}

// Loads the client into the process the debugger side was handed. Returns the result line.
std::string attach(const std::wstring& dll, DWORD pid, HANDLE thread) {
  HANDLE process = open_for_injection(pid);
  if (!process) {
    ResumeThread(thread);
    return "failed the started process could not be opened (error " +
           std::to_string(GetLastError()) + ")";
  }
  std::string outcome;
  if (is_game_image(process)) {
    // The package's entry is the game itself: it is still suspended, as with a Steam start.
    if (load_client(process, dll)) {
      ResumeThread(thread);
      outcome = "started " + std::to_string(pid);
    } else {
      TerminateProcess(process, 1);
      outcome = "failed the client could not be loaded into the game";
    }
    CloseHandle(process);
    return outcome;
  }
  // A launch helper: let it run and catch the game it starts.
  CloseHandle(process);
  ResumeThread(thread);
  const DWORD game_pid = wait_for_child_game(pid, kHelperChildTimeoutMs);
  if (game_pid == 0) {
    return "failed the package did not start KingdomCome.exe";
  }
  HANDLE game = open_for_injection(game_pid);
  if (!game) {
    return "failed the game process could not be opened (error " + std::to_string(GetLastError()) +
           ")";
  }
  if (load_client(game, dll)) {
    outcome = "started " + std::to_string(game_pid);
  } else {
    TerminateProcess(game, 1);
    outcome = "failed the client could not be loaded into the game";
  }
  CloseHandle(game);
  return outcome;
}

}  // namespace

bool is_package_attach(int argc, wchar_t** argv) {
  return argc >= 2 && wcscmp(argv[1], kAttachSwitch) == 0;
}

int attach_package_process(int argc, wchar_t** argv) {
  // <tool> --package-attach <dll> <result file> <owner pid> -p <pid> -tid <tid>
  if (argc != 9 || wcscmp(argv[5], L"-p") != 0 || wcscmp(argv[7], L"-tid") != 0) {
    return 2;
  }
  const std::wstring dll = argv[2];
  const std::wstring result = argv[3];
  const DWORD owner = wcstoul(argv[4], nullptr, 10);
  const DWORD pid = wcstoul(argv[6], nullptr, 10);
  const DWORD tid = wcstoul(argv[8], nullptr, 10);
  HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
  if (!thread) {
    return 1;
  }
  if (owner == 0 || !process_alive(owner)) {
    // Not a KCDS start: the registration outlived the start that made it. Start the game as
    // the player asked and remove the registration.
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
      wchar_t full_name[PACKAGE_FULL_NAME_MAX_LENGTH + 1]{};
      UINT32 length = PACKAGE_FULL_NAME_MAX_LENGTH + 1;
      if (GetPackageFullName(process, &length, full_name) == ERROR_SUCCESS &&
          SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        disable_debugging(full_name);
        CoUninitialize();
      }
      CloseHandle(process);
    }
    ResumeThread(thread);
    CloseHandle(thread);
    return 0;
  }
  const std::string outcome = attach(dll, pid, thread);
  CloseHandle(thread);
  write_result(result, outcome);
  return outcome.rfind("started ", 0) == 0 ? 0 : 1;
}

int launch_package(const StorePackage& package, const std::wstring& dll,
                   const std::wstring& arguments) {
  const std::wstring aumid = app_user_model_id(package);
  if (package.full_name.empty() || aumid.empty()) {
    printf("Microsoft Store copy is not installed for this Windows user (manifest %ls)\n",
           package.manifest.wstring().c_str());
    return 3;
  }
  wchar_t self[MAX_PATH * 4]{};
  const DWORD self_length = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
  wchar_t temp[MAX_PATH + 1]{};
  const DWORD temp_length = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
  if (self_length == 0 || self_length >= std::size(self) || temp_length == 0 ||
      temp_length >= std::size(temp)) {
    printf("Package start failed: no path for the starter or the temporary folder\n");
    return 1;
  }
  const std::wstring result = std::wstring(temp, temp_length) + L"kcds-package-start-" +
                              std::to_wstring(GetCurrentProcessId()) + L".txt";
  if (result.find(L'"') != std::wstring::npos) {
    return 1;
  }
  DeleteFileW(result.c_str());
  const std::wstring debugger = L"\"" + std::wstring(self, self_length) + L"\" " + kAttachSwitch +
                                L" \"" + dll + L"\" \"" + result + L"\" " +
                                std::to_wstring(GetCurrentProcessId());

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    printf("Package start failed: COM is not available\n");
    return 1;
  }
  int code = 1;
  {
    ComPointer<IPackageDebugSettings> settings;
    ComPointer<IApplicationActivationManager> activation;
    HRESULT hr = CoCreateInstance(CLSID_PackageDebugSettings, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&settings.pointer));
    if (SUCCEEDED(hr)) {
      hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                            IID_PPV_ARGS(&activation.pointer));
    }
    if (SUCCEEDED(hr)) {
      hr = settings->EnableDebugging(package.full_name.c_str(), debugger.c_str(), nullptr);
    }
    if (FAILED(hr)) {
      printf("Package start failed: Windows refused the start registration (0x%08lX)\n",
             static_cast<unsigned long>(hr));
    } else {
      printf("Starting Microsoft Store package %ls\n", aumid.c_str());
      DWORD activated = 0;
      hr = activation->ActivateApplication(aumid.c_str(), arguments.c_str(), AO_NONE, &activated);
      // The debugger side may still be running when the activation call returns or gives up.
      const std::string outcome =
          read_result(result, SUCCEEDED(hr) ? kResultTimeoutMs : kFailedActivationResultMs);
      if (outcome.rfind("started ", 0) == 0) {
        const DWORD game_pid = strtoul(outcome.c_str() + 8, nullptr, 10);
        printf("Started own client PID=%lu (Microsoft Store package)\n", game_pid);
        code = 0;
        HANDLE game = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, game_pid);
        if (game) {
          if (WaitForSingleObject(game, kEarlyExitMs) == WAIT_OBJECT_0) {
            DWORD exit_code = 0;
            GetExitCodeProcess(game, &exit_code);
            printf("The game closed right after its start (exit code %lu)\n", exit_code);
            code = 4;
          }
          CloseHandle(game);
        }
      } else if (!outcome.empty()) {
        printf("Package start %s\n", outcome.c_str());
      } else {
        printf("Package start failed: no answer from the started package (0x%08lX)\n",
               static_cast<unsigned long>(hr));
      }
    }
    // Never leave the registration behind, whatever happened above.
    if (settings.pointer) {
      settings->DisableDebugging(package.full_name.c_str());
    }
  }
  CoUninitialize();
  return code;
}

}  // namespace kcds::runtime::launch
