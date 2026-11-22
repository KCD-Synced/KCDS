// MinHook for the payload: the client's MH_* calls go to the KCDS loader (kcds_loader.dll), which
// owns every hook of the game so that one KCDS release can be unloaded and another loaded
// (Client/loader, kcds/loader/kcds_abi.h). The hook code of every feature stays as it was.
//
// The loader is injected before the payload (kcd2s-launch-client); without it no hook can be made
// and the client reports an unsupported start, as for a game function that was not found.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MinHook.h"
#include "kcds/loader/kcds_abi.h"

namespace {

const KcdsLoaderApi* loader() {
  static const KcdsLoaderApi* api = [] {
    HMODULE module = GetModuleHandleW(KCDS_LOADER_MODULE);
    auto get = module ? reinterpret_cast<KcdsGetLoaderApi>(
                            reinterpret_cast<void*>(GetProcAddress(module, KCDS_LOADER_EXPORT)))
                      : nullptr;
    return get ? get(KCDS_LOADER_API_VERSION) : nullptr;
  }();
  return api;
}

bool initialized = false;

MH_STATUS status_of(KcdsHookStatus status) {
  return static_cast<MH_STATUS>(status);
}

}  // namespace

extern "C" {

MH_STATUS WINAPI MH_Initialize(VOID) {
  if (!loader()) {
    return MH_ERROR_NOT_INITIALIZED;
  }
  if (initialized) {
    return MH_ERROR_ALREADY_INITIALIZED;
  }
  initialized = true;
  loader()->log("payload hooks through the loader");
  return MH_OK;
}

MH_STATUS WINAPI MH_Uninitialize(VOID) {
  if (!loader()) {
    return MH_ERROR_NOT_INITIALIZED;
  }
  loader()->disable_hook(nullptr);
  initialized = false;
  return MH_OK;
}

MH_STATUS WINAPI MH_CreateHook(LPVOID pTarget, LPVOID pDetour, LPVOID* ppOriginal) {
  return loader() ? status_of(loader()->create_hook(pTarget, pDetour, ppOriginal))
                  : MH_ERROR_NOT_INITIALIZED;
}

MH_STATUS WINAPI MH_CreateHookApiEx(LPCWSTR pszModule, LPCSTR pszProcName, LPVOID pDetour,
                                    LPVOID* ppOriginal, LPVOID* ppTarget) {
  HMODULE module = GetModuleHandleW(pszModule);
  if (!module) {
    return MH_ERROR_MODULE_NOT_FOUND;
  }
  void* target = reinterpret_cast<void*>(GetProcAddress(module, pszProcName));
  if (!target) {
    return MH_ERROR_FUNCTION_NOT_FOUND;
  }
  if (ppTarget) {
    *ppTarget = target;
  }
  return MH_CreateHook(target, pDetour, ppOriginal);
}

MH_STATUS WINAPI MH_CreateHookApi(LPCWSTR pszModule, LPCSTR pszProcName, LPVOID pDetour,
                                  LPVOID* ppOriginal) {
  return MH_CreateHookApiEx(pszModule, pszProcName, pDetour, ppOriginal, nullptr);
}

MH_STATUS WINAPI MH_RemoveHook(LPVOID pTarget) {
  return loader() ? status_of(loader()->remove_hook(pTarget)) : MH_ERROR_NOT_INITIALIZED;
}

MH_STATUS WINAPI MH_EnableHook(LPVOID pTarget) {
  return loader() ? status_of(loader()->enable_hook(pTarget)) : MH_ERROR_NOT_INITIALIZED;
}

MH_STATUS WINAPI MH_DisableHook(LPVOID pTarget) {
  return loader() ? status_of(loader()->disable_hook(pTarget)) : MH_ERROR_NOT_INITIALIZED;
}

MH_STATUS WINAPI MH_QueueEnableHook(LPVOID pTarget) {
  return loader() ? status_of(loader()->queue_enable_hook(pTarget)) : MH_ERROR_NOT_INITIALIZED;
}

MH_STATUS WINAPI MH_QueueDisableHook(LPVOID pTarget) {
  return loader() ? status_of(loader()->queue_disable_hook(pTarget)) : MH_ERROR_NOT_INITIALIZED;
}

MH_STATUS WINAPI MH_ApplyQueued(VOID) {
  return loader() ? status_of(loader()->apply_queued()) : MH_ERROR_NOT_INITIALIZED;
}

const char* WINAPI MH_StatusToString(MH_STATUS status) {
  switch (status) {
    case MH_UNKNOWN:
      return "MH_UNKNOWN";
    case MH_OK:
      return "MH_OK";
    case MH_ERROR_ALREADY_INITIALIZED:
      return "MH_ERROR_ALREADY_INITIALIZED";
    case MH_ERROR_NOT_INITIALIZED:
      return "MH_ERROR_NOT_INITIALIZED";
    case MH_ERROR_ALREADY_CREATED:
      return "MH_ERROR_ALREADY_CREATED";
    case MH_ERROR_NOT_CREATED:
      return "MH_ERROR_NOT_CREATED";
    case MH_ERROR_ENABLED:
      return "MH_ERROR_ENABLED";
    case MH_ERROR_DISABLED:
      return "MH_ERROR_DISABLED";
    case MH_ERROR_NOT_EXECUTABLE:
      return "MH_ERROR_NOT_EXECUTABLE";
    case MH_ERROR_UNSUPPORTED_FUNCTION:
      return "MH_ERROR_UNSUPPORTED_FUNCTION";
    case MH_ERROR_MEMORY_ALLOC:
      return "MH_ERROR_MEMORY_ALLOC";
    case MH_ERROR_MEMORY_PROTECT:
      return "MH_ERROR_MEMORY_PROTECT";
    case MH_ERROR_MODULE_NOT_FOUND:
      return "MH_ERROR_MODULE_NOT_FOUND";
    case MH_ERROR_FUNCTION_NOT_FOUND:
      return "MH_ERROR_FUNCTION_NOT_FOUND";
  }
  return "(unknown)";
}

}  // extern "C"
