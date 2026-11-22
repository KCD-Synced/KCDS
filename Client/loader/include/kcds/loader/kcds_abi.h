/* The interface between the KCDS loader (kcds_loader.dll) and a KCDS payload (the game client of
 * one KCDS release, versions\<version>\ or next to the loader).
 *
 * The loader is injected first and stays for the whole game: it owns every hook of the game, so
 * one payload can be unloaded and another loaded (a server of another KCDS release) without the
 * game noticing. The payload never patches the game itself: its MinHook calls go to the loader
 * (Client/runtime/loader_link/minhook_bridge.cpp), which gives each hooked game function a thunk
 * that jumps to the payload's detour, or to the game's own code while no payload has it.
 *
 * Plain C across the DLL boundary: no C++ types, no exceptions, no memory freed on the other side.
 * LoaderApi only ever grows at its end; KCDS_LOADER_API_VERSION counts its revisions, and a payload
 * asks for the oldest revision it needs. */
#ifndef KCDS_LOADER_KCDS_ABI_H
#define KCDS_LOADER_KCDS_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KCDS_LOADER_API_VERSION 3u

/* The loader's module name and its one export: const KcdsLoaderApi* kcds_get_loader_api(uint32_t
 * version), null when the loader is older than that version. */
#define KCDS_LOADER_MODULE L"kcds_loader.dll"
#define KCDS_LOADER_EXPORT "kcds_get_loader_api"

/* Hook results: MinHook's MH_STATUS values. */
typedef int KcdsHookStatus;

/* What a payload gives the loader once it runs (register_payload). */
typedef struct KcdsPayloadApi {
  uint32_t version; /* KCDS_LOADER_API_VERSION the payload was built with */
  /* Stops everything of the payload before it is unloaded: its threads joined, its views and
   * windows closed, what it registered with the game and Windows taken back, its child processes
   * ended. 0 when done; any other value stops the swap and the game keeps this payload. Called on
   * the loader's own thread, with the game in its main menu. */
  int (*prepare_unload)(void);
} KcdsPayloadApi;

typedef struct KcdsLoaderApi {
  uint32_t version; /* KCDS_LOADER_API_VERSION of the loader */

  /* Hooks, as MinHook's: target is the game function, detour the payload's; *original receives the
   * function that runs the game's own code. A target another payload hooked before keeps its
   * thunk and original. A null target (MH_ALL_HOOKS) in enable/disable means every hook of the
   * payload. */
  KcdsHookStatus (*create_hook)(void* target, void* detour, void** original);
  KcdsHookStatus (*enable_hook)(void* target);
  KcdsHookStatus (*disable_hook)(void* target);
  KcdsHookStatus (*remove_hook)(void* target);
  KcdsHookStatus (*queue_enable_hook)(void* target);
  KcdsHookStatus (*queue_disable_hook)(void* target);
  KcdsHookStatus (*apply_queued)(void);

  /* A line in the loader's log (UTF-8). */
  void (*log)(const char* line);

  /* Since version 2: the payload's swap. */
  /* The payload that runs now (its module and what it gives the loader). */
  void (*register_payload)(void* module, const KcdsPayloadApi* payload);
  /* Unloads the payload and loads the one at path (UTF-16) on the loader's own thread. join: the
   * server the new payload joins once its menu is up (host:port or a list id), or empty. 0 when
   * the swap is under way. */
  int (*request_swap)(const wchar_t* path, const char* join);
  /* The join the last swap handed over, once; empty when none. */
  const char* (*take_join)(void);

  /* Since version 3: the game's code as it was before any hook. copy holds the size bytes at base
   * (the game module copied by the payload); the bytes the loader patched there are written back.
   * The number of patches written back (a payload loaded after others scans such a copy: its
   * signatures name the game's own bytes, not the hooks'). */
  uint32_t (*restore_original_bytes)(const void* base, uint64_t size, uint8_t* copy);
} KcdsLoaderApi;

typedef const KcdsLoaderApi* (*KcdsGetLoaderApi)(uint32_t version);

#ifdef __cplusplus
}
#endif

#endif /* KCDS_LOADER_KCDS_ABI_H */
