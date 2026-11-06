// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "carcass_prompts.hpp"

#include <cstdint>
#include <cstring>

#include "MinHook.h"
#include "npc_health.hpp"

namespace kcds::runtime::npcs::carcass_prompts {
namespace {

constexpr char kContext[] = "animal_disableLootButcherActions";

using HasScriptContext = std::int64_t(__fastcall*)(void* binding, void* handler, const char* name);
using SoulOfHandler = void*(__fastcall*)(void* handler);
using ReturnBool = std::int64_t(__fastcall*)(void* handler, const std::uint64_t* value);

HasScriptContext original = nullptr;
SoulOfHandler soul_of_handler = nullptr;
ReturnBool return_bool = nullptr;

std::int64_t __fastcall hooked(void* binding, void* handler, const char* name) {
  if (name && std::strcmp(name, kContext) == 0) {
    void* soul = soul_of_handler(handler);
    const auto body = soul ? health::body_of_soul(soul) : std::nullopt;
    if (body && health::carcass_closed(body->npc)) {
      const std::uint64_t yes = 1;
      return return_bool(handler, &yes);
    }
  }
  return original(binding, handler, name);
}

}  // namespace

bool install(const Natives& natives, Logger logger) {
  soul_of_handler = reinterpret_cast<SoulOfHandler>(natives.soul_of_handler);
  return_bool = reinterpret_cast<ReturnBool>(natives.return_bool);
  const bool hooked_it = natives.has_script_context && soul_of_handler && return_bool &&
                         MH_CreateHook(natives.has_script_context, reinterpret_cast<void*>(&hooked),
                                       reinterpret_cast<void**>(&original)) == MH_OK &&
                         MH_EnableHook(natives.has_script_context) == MH_OK;
  if (logger) {
    logger("NPC carcass prompts %s", hooked_it ? "hooked" : "unavailable");
  }
  return hooked_it;
}

}  // namespace kcds::runtime::npcs::carcass_prompts
