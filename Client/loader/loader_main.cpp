// kcds_loader.dll: injected into the game before the payload (kcd2s-launch-client), and never
// unloaded. It owns the game's hooks (hook_registry.hpp) and hands them out through its one
// export, kcds_get_loader_api (kcds/loader/kcds_abi.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

#include "cef_host.hpp"
#include "discord/discord_link.hpp"
#include "hook_registry.hpp"
#include "kcds/loader/kcds_abi.h"
#include "minhook_backend.hpp"

namespace {

kcds::loader::MinHookBackend backend;
kcds::loader::HookRegistry hooks(backend);
std::mutex log_mutex;

// %LOCALAPPDATA%\KCDS\logs\loader.log, a line each, appended.
void write_log(const char* line) {
  std::lock_guard<std::mutex> lock(log_mutex);
  wchar_t local[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0) {
    return;
  }
  const std::wstring path = std::wstring(local) + L"\\KCDS\\logs\\loader.log";
  FILE* out = nullptr;
  if (_wfopen_s(&out, path.c_str(), L"ab") != 0 || !out) {
    return;
  }
  SYSTEMTIME now;
  GetLocalTime(&now);
  std::fprintf(out, "[%02u:%02u:%02u.%03u %lu] %s\n", now.wHour, now.wMinute, now.wSecond,
               now.wMilliseconds, GetCurrentProcessId(), line ? line : "");
  std::fclose(out);
}

KcdsHookStatus create_hook(void* target, void* detour, void** original) {
  return hooks.create_hook(target, detour, original);
}
KcdsHookStatus enable_hook(void* target) {
  return hooks.enable_hook(target);
}
KcdsHookStatus disable_hook(void* target) {
  return hooks.disable_hook(target);
}
KcdsHookStatus remove_hook(void* target) {
  return hooks.remove_hook(target);
}
KcdsHookStatus queue_enable_hook(void* target) {
  return hooks.queue_enable_hook(target);
}
KcdsHookStatus queue_disable_hook(void* target) {
  return hooks.queue_disable_hook(target);
}
KcdsHookStatus apply_queued() {
  return hooks.apply_queued();
}

// The payload that runs now, and a swap under way.
std::mutex payload_mutex;
HMODULE payload_module = nullptr;
const KcdsPayloadApi* payload_api = nullptr;
bool swapping = false;
uint32_t swap_count = 0;
std::wstring swap_path;
std::string handed_join;
std::string taken_join;
// How long the game's threads get to leave the payload's detours once the hooks run the game's
// own code again, before its code is unmapped.
constexpr DWORD kDrainMs = 500;
// How long the old payload's unload may take before the loader logs where it waits.
constexpr int kUnloadWatchMs = 5000;

void log_line(const std::string& line) {
  write_log(line.c_str());
}

std::string narrow(const std::wstring& text) {
  std::string out;
  for (wchar_t c : text) {
    out.push_back(c < 128 ? char(c) : '?');
  }
  return out;
}

void register_payload(void* module, const KcdsPayloadApi* payload) {
  std::lock_guard<std::mutex> lock(payload_mutex);
  char at[64];
  std::snprintf(at, sizeof(at), "payload at %p", module);
  write_log(at);
  payload_module = static_cast<HMODULE>(module);
  payload_api = payload;
  char line[96];
  std::snprintf(line, sizeof(line), "payload registered (interface %u)",
                payload ? unsigned(payload->version) : 0u);
  write_log(line);
}

// Every thread of the game whose stack has a frame in [base, base + size): its id and the first
// such frames (module offsets). Each is suspended for the moment of its unwind.
// KCDS_SWAP_TRAP: the ranges of the old payloads kept mapped with no access.
struct TrappedRange {
  std::uintptr_t start = 0;
  std::uintptr_t end = 0;
};
TrappedRange trapped[64];
std::atomic<int> trapped_count{0};
std::atomic<int> trap_reports{0};

bool in_trap(std::uintptr_t address) {
  const int count = trapped_count.load();
  for (int i = 0; i < count; ++i) {
    if (address >= trapped[i].start && address < trapped[i].end) {
      return true;
    }
  }
  return false;
}

// "module+0xoffset" for a code address (the trapped payloads are still in the module list).
void describe_address(std::uintptr_t address, char* text, std::size_t size) {
  HMODULE module = nullptr;
  wchar_t path[MAX_PATH] = {};
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(address), &module) &&
      GetModuleFileNameW(module, path, MAX_PATH)) {
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    std::snprintf(
        text, size, "%ls+0x%llx%s", name,
        static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)),
        in_trap(address) ? "(old)" : "");
  } else {
    std::snprintf(text, size, "%p", reinterpret_cast<void*>(address));
  }
}

// KCDS_SWAP_TRAP: an access into an old payload is logged with who did it (the faulting code,
// the return address on the stack and the code addresses further up the stack), then left to the
// game's own handling.
LONG CALLBACK report_trapped_access(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const auto rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
  const auto target = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
  if (!in_trap(rip) && !in_trap(target)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (trap_reports.fetch_add(1) >= 4) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  char line[1024];
  char where[160];
  describe_address(rip, where, sizeof(where));
  int used = std::snprintf(line, sizeof(line),
                           "TRAP thread %lu: access to %p at %s; stack:", GetCurrentThreadId(),
                           reinterpret_cast<void*>(target), where);
  const auto* stack = reinterpret_cast<const std::uintptr_t*>(info->ContextRecord->Rsp);
  int shown = 0;
  for (int i = 0; i < 256 && shown < 10 && used < 900; ++i) {
    const std::uintptr_t value = stack[i];
    HMODULE module = nullptr;
    if (value < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               reinterpret_cast<LPCWSTR>(value), &module)) {
      continue;
    }
    describe_address(value, where, sizeof(where));
    used += std::snprintf(line + used, sizeof(line) - used, " %s", where);
    ++shown;
  }
  write_log(line);
  return EXCEPTION_CONTINUE_SEARCH;
}

// The threads with a frame in [base, base + size), each logged (with its frames) when log is set.
int count_threads_in(const std::uint8_t* base, std::size_t size, bool log) {
  int with_frames = 0;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return 0;
  }
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  const DWORD process = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) {
      continue;
    }
    HANDLE thread =
        OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                   entry.th32ThreadID);
    if (!thread) {
      continue;
    }
    // While the thread is suspended nothing here allocates or takes a lock it may hold (the
    // heap's): the offsets go into a fixed array, and the text is made once it runs again.
    std::uintptr_t offsets[8] = {};
    int found = 0;
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT context{};
      context.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(thread, &context)) {
        for (int depth = 0; depth < 48 && context.Rip; ++depth) {
          const auto rip = static_cast<std::uintptr_t>(context.Rip);
          const auto start = reinterpret_cast<std::uintptr_t>(base);
          if (rip >= start && rip < start + size && found < 8) {
            offsets[found++] = rip - start;
          }
          DWORD64 image_base = 0;
          PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
          if (!function) {
            // A leaf function: its return address is at the top of the stack.
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
          }
          void* handler_data = nullptr;
          DWORD64 frame = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                           &handler_data, &frame, nullptr);
        }
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
    std::string frames;
    for (int i = 0; i < found; ++i) {
      char offset[24];
      std::snprintf(offset, sizeof(offset), " +0x%llx",
                    static_cast<unsigned long long>(offsets[i]));
      frames += offset;
    }
    if (!frames.empty()) {
      ++with_frames;
      if (log) {
        log_line("  thread " + std::to_string(entry.th32ThreadID) + ":" + frames);
      }
    }
  }
  CloseHandle(snapshot);
  return with_frames;
}

int log_threads_in(const std::uint8_t* base, std::size_t size) {
  return count_threads_in(base, size, true);
}

// The loader's own thread: the old payload stops and goes, the new one is loaded and starts itself
// (its DllMain), registering again once it runs.
// A watchdog started with each swap, before the payload's unload: once that unload (its static
// destructors and DLL detach, under the loader lock, where no new thread can start) has not
// returned in a while, the frames it waits in are logged.
struct UnloadWatch {
  std::atomic<ULONGLONG> since{0};
  std::atomic<bool> swap_done{false};
  const std::uint8_t* base = nullptr;
  std::size_t size = 0;
};
UnloadWatch unload_watch;

DWORD WINAPI watch_unload(void*) {
  bool logged = false;
  while (!unload_watch.swap_done.load()) {
    const ULONGLONG since = unload_watch.since.load();
    if (since != 0 && !logged && GetTickCount64() - since > ULONGLONG(kUnloadWatchMs)) {
      logged = true;
      log_line("swap: the old payload's unload has not returned; its threads:");
      log_threads_in(unload_watch.base, unload_watch.size);
    }
    Sleep(100);
  }
  return 0;
}

DWORD WINAPI run_swap(void*) {
  unload_watch.swap_done.store(false);
  unload_watch.since.store(0);
  HANDLE watcher = CreateThread(nullptr, 0, &watch_unload, nullptr, 0, nullptr);
  struct EndWatch {
    HANDLE thread;
    ~EndWatch() {
      unload_watch.swap_done.store(true);
      if (thread) {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
      }
    }
  } end_watch{watcher};
  HMODULE module = nullptr;
  const KcdsPayloadApi* payload = nullptr;
  std::wstring path;
  {
    std::lock_guard<std::mutex> lock(payload_mutex);
    module = payload_module;
    payload = payload_api;
    path = swap_path;
  }
  // Its hooks first run the game's own code, and the game's threads leave its detours; then its
  // unload steps run with nothing of it called by the game.
  log_line("swap: the payload's hooks pause");
  hooks.pause_payload();
  Sleep(kDrainMs);
  log_line("swap: the payload stops");
  const int stopped = payload && payload->prepare_unload ? payload->prepare_unload() : 0;
  if (stopped != 0) {
    log_line("swap refused by the payload (" + std::to_string(stopped) + "); it keeps running");
    hooks.resume_payload();
    std::lock_guard<std::mutex> lock(payload_mutex);
    swapping = false;
    return 0;
  }
  MODULEINFO mapped{};
  if (module) {
    GetModuleInformation(GetCurrentProcess(), module, &mapped, sizeof(mapped));
  }
  // Hooks of the payload's own code or pointers go for good (that memory is freed next, and a later
  // module may be mapped at the same place); the game's stay for the next payload.
  const int removed = hooks.release_payload(mapped.lpBaseOfDll, mapped.SizeOfImage);
  log_line("swap: " + std::to_string(removed) + " hook(s) inside the payload removed");
  // No thread of the game may still be inside the payload's code: a hook whose call of the game's
  // own function runs long (a loop that never returns) would come back into code that is gone.
  // One still there (a call that has not come back yet) gets a few seconds; if it stays, the
  // payload is not freed: a thread coming back into freed code, or into the next payload mapped at
  // the same place, crashes the game.
  bool keep_mapped = false;
  if (module) {
    int inside = 0;
    for (int check = 0; check < 50; ++check) {
      if (check == 0 || check == 49) {
        log_line("swap: threads still inside the payload:");
      }
      inside = count_threads_in(static_cast<const std::uint8_t*>(mapped.lpBaseOfDll),
                                mapped.SizeOfImage, check == 0 || check == 49);
      if (inside == 0) {
        break;
      }
      Sleep(100);
    }
    log_line("swap: " + std::to_string(inside) + " thread(s) inside the payload");
    keep_mapped = inside > 0;
  }
  {
    std::lock_guard<std::mutex> lock(payload_mutex);
    payload_module = nullptr;
    payload_api = nullptr;
  }
  if (module) {
    MODULEINFO info{};
    GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info));
    wchar_t old_path[MAX_PATH] = {};
    GetModuleFileNameW(module, old_path, MAX_PATH);
    // Test hook (KCDS_SWAP_TRAP=1): the old payload stays mapped with no access, so the next one
    // loads elsewhere and anything that still calls the old one faults at once in its range (the
    // address names what was left behind); without it the same code at the same address ran.
    if (GetEnvironmentVariableW(L"KCDS_SWAP_TRAP", nullptr, 0) > 0) {
      DWORD old = 0;
      VirtualProtect(info.lpBaseOfDll, info.SizeOfImage, PAGE_NOACCESS, &old);
      const int slot = trapped_count.load();
      if (slot < 64) {
        trapped[slot].start = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        trapped[slot].end = trapped[slot].start + info.SizeOfImage;
        trapped_count.store(slot + 1);
      }
      static const PVOID reporter = AddVectoredExceptionHandler(1, report_trapped_access);
      (void)reporter;
      char line[128];
      std::snprintf(
          line, sizeof(line), "swap: trap: the old payload stays at %p..%p, no access",
          info.lpBaseOfDll,
          static_cast<void*>(static_cast<std::uint8_t*>(info.lpBaseOfDll) + info.SizeOfImage));
      write_log(line);
      module = nullptr;
    } else if (keep_mapped) {
      // A thread is still inside (see above): the payload stays loaded, unused; the next one is
      // mapped elsewhere. Its memory is the price of not crashing the game.
      log_line("swap: the old payload stays loaded: a thread is still inside it");
      module = nullptr;
    }
    unload_watch.base = static_cast<const std::uint8_t*>(info.lpBaseOfDll);
    unload_watch.size = info.SizeOfImage;
    unload_watch.since.store(GetTickCount64());
    if (module) {
      FreeLibrary(module);
    }
    unload_watch.since.store(0);
    if (!module) {
      // KCDS_SWAP_TRAP, or a thread still inside: kept mapped on purpose.
    } else if (GetModuleHandleW(old_path)) {
      // Still mapped: a thread the payload started (the C runtime keeps the module of a running
      // std::thread loaded) has not ended. Its code keeps running; the threads are named here.
      log_line("swap: the old payload is still mapped; its threads still running:");
      log_threads_in(static_cast<const std::uint8_t*>(info.lpBaseOfDll), info.SizeOfImage);
    } else {
      log_line("swap: the old payload is unloaded");
    }
  }
  {
    std::lock_guard<std::mutex> lock(payload_mutex);
    ++swap_count;
  }
  HMODULE loaded = LoadLibraryW(path.c_str());
  if (loaded) {
    char line[64];
    std::snprintf(line, sizeof(line), "swap: the new payload is at %p", static_cast<void*>(loaded));
    write_log(line);
  }
  log_line(std::string("swap: ") + (loaded ? "loaded " : "could not load ") + narrow(path) +
           (loaded ? "" : " (error " + std::to_string(GetLastError()) + ")"));
  std::lock_guard<std::mutex> lock(payload_mutex);
  swapping = false;
  return 0;
}

int request_swap(const wchar_t* path, const char* join) {
  {
    std::lock_guard<std::mutex> lock(payload_mutex);
    if (swapping || !path || !*path) {
      return 1;
    }
    swapping = true;
    swap_path = path;
    handed_join = join ? join : "";
  }
  log_line("swap requested to " + narrow(path) +
           (join && *join ? " to join " + std::string(join) : ""));
  HANDLE thread = CreateThread(nullptr, 0, &run_swap, nullptr, 0, nullptr);
  if (!thread) {
    std::lock_guard<std::mutex> lock(payload_mutex);
    swapping = false;
    return 1;
  }
  CloseHandle(thread);
  return 0;
}

uint32_t restore_original_bytes(const void* base, uint64_t size, uint8_t* copy) {
  return backend.restore_original_bytes(base, size, copy);
}

int cef_start(const KcdsCefSettings* settings) {
  return kcds::loader::cef::start(settings, &write_log);
}

int cef_ready() {
  return kcds::loader::cef::ready();
}

KcdsHookStatus hook_pointer(void** where, void* detour, void** original) {
  return hooks.hook_pointer(where, detour, original);
}

void discord_start(const char* app_id) {
  kcds::loader::discord::start(app_id ? app_id : "", &write_log);
}

void discord_set_activity(const char* activity_json) {
  kcds::loader::discord::set_activity(activity_json ? activity_json : "");
}

// Copies text into a client's buffer of size bytes, NUL-ended (cut when it does not fit).
void copy_out(const std::string& text, char* into, uint32_t size) {
  if (!into || size == 0) {
    return;
  }
  const std::size_t count = text.size() < size ? text.size() : size - 1;
  std::memcpy(into, text.data(), count);
  into[count] = '\0';
}

int discord_status(char* user_id, uint32_t size) {
  std::string user;
  const bool connected = kcds::loader::discord::status(user);
  copy_out(user, user_id, size);
  return connected ? 1 : 0;
}

int discord_authorize(int ask) {
  return kcds::loader::discord::authorize(ask != 0) ? 1 : 0;
}

int discord_authorization(char* code, uint32_t size) {
  std::string answer;
  const int state = kcds::loader::discord::authorization(answer);
  copy_out(answer, code, size);
  return state;
}

uint32_t swaps() {
  std::lock_guard<std::mutex> lock(payload_mutex);
  return swap_count;
}

const char* take_join() {
  std::lock_guard<std::mutex> lock(payload_mutex);
  taken_join = handed_join;
  handed_join.clear();
  return taken_join.c_str();
}

const KcdsLoaderApi api = {
    KCDS_LOADER_API_VERSION,
    &create_hook,
    &enable_hook,
    &disable_hook,
    &remove_hook,
    &queue_enable_hook,
    &queue_disable_hook,
    &apply_queued,
    &write_log,
    &register_payload,
    &request_swap,
    &take_join,
    &restore_original_bytes,
    &cef_start,
    &cef_ready,
    &swaps,
    &hook_pointer,
    &discord_start,
    &discord_set_activity,
    &discord_status,
    &discord_authorize,
    &discord_authorization,
};

}  // namespace

extern "C" __declspec(dllexport) const KcdsLoaderApi* kcds_get_loader_api(uint32_t version) {
  return version <= KCDS_LOADER_API_VERSION ? &api : nullptr;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    char line[96];
    std::snprintf(line, sizeof(line), "KCDS loader started (interface %u)",
                  unsigned(KCDS_LOADER_API_VERSION));
    write_log(line);
  }
  return TRUE;
}
