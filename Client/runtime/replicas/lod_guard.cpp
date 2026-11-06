#include "lod_guard.hpp"

#include <cstdint>
#include <cstring>

#include "MinHook.h"

namespace kcds::runtime::replicas {
namespace {

using LodUpdate = void*(__fastcall*)(void* manager, void* key, char flag);
using LodEntry = void*(__fastcall*)(void* manager, void* key);

// calls its entry lookup (the thunk) at + 21: E8 and a rel32.
constexpr std::size_t kLookupCall = 21;

LodUpdate original_update = nullptr;
LodEntry lookup = nullptr;
LodLogger write_log = nullptr;
bool logged = false;

void* __fastcall guarded_update(void* manager, void* key, char flag) {
  if (!lookup(manager, key)) {
    if (write_log && !logged) {
      logged = true;
      write_log("REPLICA LOD update skipped for an actor without an AI LOD entry");
    }
    return nullptr;
  }
  return original_update(manager, key, flag);
}

}  // namespace

bool install_lod_guard(void* lod_update, LodLogger logger) {
  write_log = logger;
  if (!lod_update) {
    return false;
  }
  auto* code = static_cast<std::uint8_t*>(lod_update);
  if (code[kLookupCall] != 0xE8) {
    return false;
  }
  std::int32_t relative = 0;
  std::memcpy(&relative, code + kLookupCall + 1, sizeof(relative));
  lookup = reinterpret_cast<LodEntry>(code + kLookupCall + 5 + relative);
  return MH_CreateHook(lod_update, reinterpret_cast<void*>(guarded_update),
                       reinterpret_cast<void**>(&original_update)) == MH_OK &&
         MH_EnableHook(lod_update) == MH_OK;
}

}  // namespace kcds::runtime::replicas
