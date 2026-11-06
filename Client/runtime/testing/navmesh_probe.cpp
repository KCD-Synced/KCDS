#include "../core/client_internal.hpp"

#include "navmesh_probe.hpp"

#include <cstdint>

#include "../core/log.hpp"
#include "MinHook.h"

namespace kcds::runtime::testing::navmesh_probe {
namespace {

using kcds::game_client::log;

// WHGame.dll 1.5.5 at its. A test-only probe names what it uses by address.
constexpr std::uintptr_t kIdaBase = 0x180000000ULL;
constexpr std::uintptr_t kAgentLoad = 0x180D38E40ULL;
constexpr std::uintptr_t kIndexRead = 0x1807E5B64ULL;
constexpr std::uintptr_t kTilesRead = 0x1807E512CULL;

using AgentLoad = char(__fastcall*)(void* navigation);
using IndexRead = char(__fastcall*)(void* entries, void* path);
using TilesRead = char(__fastcall*)(void* serializer);

AgentLoad original_agent_load = nullptr;
IndexRead original_index_read = nullptr;
TilesRead original_tiles_read = nullptr;

char __fastcall hooked_agent_load(void* navigation) {
  log("NAVPROBE agent load");
  const char loaded = original_agent_load(navigation);
  log("NAVPROBE agent load done %d", int(loaded));
  return loaded;
}

// The path is a CryStringT: its first field is the text.
char __fastcall hooked_index_read(void* entries, void* path) {
  const char* text = path ? *static_cast<const char* const*>(path) : nullptr;
  const char read = original_index_read(entries, path);
  log("NAVPROBE index %s %s", text ? text : "(none)", read ? "ok" : "FAILED");
  return read;
}

char __fastcall hooked_tiles_read(void* serializer) {
  const char read = original_tiles_read(serializer);
  log("NAVPROBE tiles %s", read ? "ok" : "FAILED");
  return read;
}

template <class Function>
bool hook(std::uintptr_t ida_address, Function replacement, Function* original) {
  const HMODULE game = GetModuleHandleW(L"WHGame.dll");
  if (!game) {
    return false;
  }
  void* target =
      reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(game) + (ida_address - kIdaBase));
  return MH_CreateHook(target, reinterpret_cast<void*>(replacement),
                       reinterpret_cast<void**>(original)) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

void install() {
  const bool agent = hook(kAgentLoad, &hooked_agent_load, &original_agent_load);
  const bool index = hook(kIndexRead, &hooked_index_read, &original_index_read);
  const bool tiles = hook(kTilesRead, &hooked_tiles_read, &original_tiles_read);
  log("NAVPROBE installed agent=%d index=%d tiles=%d", int(agent), int(index), int(tiles));
}

}  // namespace kcds::runtime::testing::navmesh_probe
