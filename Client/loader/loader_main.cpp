// kcds_loader.dll: injected into the game before the payload (kcd2s-launch-client), and never
// unloaded. It owns the game's hooks (hook_registry.hpp) and hands them out through its one
// export, kcds_get_loader_api (kcds/loader/kcds_abi.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

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

const KcdsLoaderApi api = {
    KCDS_LOADER_API_VERSION, &create_hook,        &enable_hook,  &disable_hook, &remove_hook,
    &queue_enable_hook,      &queue_disable_hook, &apply_queued, &write_log,
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
