#include "permits.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstring>

#include "../props/prop_entity.hpp"
#include "MinHook.h"

namespace kcds::runtime::crafting::permits {
namespace {

namespace wire = kcd2s::integration;
namespace entity = props::entity;

using Start = std::uint64_t(__fastcall*)(void* binding, void* handle, std::uint32_t user,
                                         std::uint32_t station);
using EndFunction = std::uint64_t(__fastcall*)(void* handle);
constexpr std::size_t kEndFunction = 11;
// An ask the server did not answer within this long is forgotten (milliseconds).
constexpr std::uint64_t kAskLifetimeMs = 10000;
constexpr std::size_t kAsksMax = 4;

// A script function handle of our own for a start the server allowed: its EndFunction does
// nothing.
std::uint64_t __fastcall no_end(void*) {
  return 0;
}
std::array<void*, 16> own_handle_table = [] {
  std::array<void*, 16> table{};
  table.fill(reinterpret_cast<void*>(&no_end));
  return table;
}();
struct OwnHandle {
  void** table = own_handle_table.data();
} own_handle;

// The player's minigame request: its component, the request (the station's id, the
// kind at + 4).
using StartMinigame = bool(__fastcall*)(void* component, std::uint8_t* request);
constexpr std::size_t kRequestKind = 4;
constexpr std::size_t kRequestBytes = 16;
// E_MinigameType: Sharpening 1, Alchemy 3, Blacksmithing 12.
constexpr std::uint8_t kSharpeningMinigame = 1;
constexpr std::uint8_t kAlchemyMinigame = 3;
constexpr std::uint8_t kSmithingMinigame = 12;

struct Asked {
  std::uint32_t request = 0;
  wire::CraftKind craft = wire::CraftKind::Alchemy;
  std::uint32_t station = 0;
  std::uint64_t at = 0;
  // A script binding's start: the binding and the user.
  void* binding = nullptr;
  std::uint32_t user = 0;
  // A minigame request: the player's component, where its request was and what it held.
  void* component = nullptr;
  std::uint8_t* request_at = nullptr;
  std::array<std::uint8_t, kRequestBytes> request_bytes{};
};

enum class Gate { Open, Closed, Asked };

Natives natives;
StartMinigame original_minigame = nullptr;
Start original_smithing = nullptr;
Start original_alchemy = nullptr;
std::uint32_t local_id = 0;
bool in_world = false;
std::uint32_t next_request = 1;
std::vector<Asked> asked;
std::vector<wire::CraftReport> to_send;
std::uint32_t refused_station = 0;

template <class... Args>
void log_line(const char* format, Args... args) {
  if (natives.log) {
    natives.log(format, args...);
  }
}

std::uint64_t end_call(void* handle) {
  auto table = *static_cast<void***>(handle);
  return reinterpret_cast<EndFunction>(table[kEndFunction])(handle);
}

// Whether the local player's start at a station goes on: closed while another player works there,
// asked of the server while in its world (the start then waits for the answer), else open.
Gate gate(Asked one, bool local) {
  void* thing = entity::by_id(one.station);
  float where[3] = {};
  if (thing) {
    entity::world_position(thing, where);
  }
  // A grindstone is not held by one player.
  const auto taken = one.craft == wire::CraftKind::Smithing  ? natives.smithy_taken
                     : one.craft == wire::CraftKind::Alchemy ? natives.table_taken
                                                             : nullptr;
  if (thing && taken && taken(where)) {
    if (refused_station != one.station) {
      refused_station = one.station;
      log_line("CRAFTING station %u is taken by another player", one.station);
    }
    return Gate::Closed;
  }
  refused_station = 0;
  if (!in_world || !local || !thing) {
    return Gate::Open;
  }
  const std::uint64_t now = GetTickCount64();
  std::erase_if(asked, [&](const Asked& old) { return now - old.at > kAskLifetimeMs; });
  if (asked.size() >= kAsksMax) {
    return Gate::Closed;
  }
  one.request = next_request++;
  one.at = now;
  asked.push_back(one);
  wire::CraftReport report;
  report.kind = wire::CraftReport::Kind::Ask;
  report.request = one.request;
  report.craft = one.craft;
  report.station = {where[0], where[1], where[2]};
  to_send.push_back(report);
  log_line("CRAFTING asked to open station %u (request %u)", one.station, one.request);
  return Gate::Asked;
}

bool __fastcall hooked_minigame(void* component, std::uint8_t* request) {
  const std::uint8_t kind = request ? request[kRequestKind] : 0;
  if (kind != kAlchemyMinigame && kind != kSmithingMinigame && kind != kSharpeningMinigame) {
    return original_minigame(component, request);
  }
  Asked one;
  one.craft = kind == kSmithingMinigame     ? wire::CraftKind::Smithing
              : kind == kSharpeningMinigame ? wire::CraftKind::Grinding
                                            : wire::CraftKind::Alchemy;
  std::memcpy(&one.station, request, sizeof(one.station));
  one.component = component;
  one.request_at = request;
  std::memcpy(one.request_bytes.data(), request, kRequestBytes);
  // Only the player's own component asks for a minigame.
  if (gate(one, true) == Gate::Open) {
    return original_minigame(component, request);
  }
  return false;
}

std::uint64_t script_start(wire::CraftKind craft, Start original, void* binding, void* handle,
                           std::uint32_t user, std::uint32_t station) {
  Asked one;
  one.craft = craft;
  one.station = station;
  one.binding = binding;
  one.user = user;
  if (gate(one, user == local_id) == Gate::Open) {
    return original(binding, handle, user, station);
  }
  return end_call(handle);
}

std::uint64_t __fastcall hooked_smithing(void* binding, void* handle, std::uint32_t user,
                                         std::uint32_t station) {
  return script_start(wire::CraftKind::Smithing, original_smithing, binding, handle, user, station);
}

std::uint64_t __fastcall hooked_alchemy(void* binding, void* handle, std::uint32_t user,
                                        std::uint32_t station) {
  return script_start(wire::CraftKind::Alchemy, original_alchemy, binding, handle, user, station);
}

template <class Fn>
void hook(void* target, void* detour, Fn* original, const char* name) {
  if (!target) {
    log_line("CRAFTING %s is missing", name);
    return;
  }
  if (MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    *original = nullptr;
    log_line("CRAFTING could not hook %s %p", name, target);
  }
}

}  // namespace

void install(const Natives& given) {
  natives = given;
  hook(natives.start_minigame, reinterpret_cast<void*>(&hooked_minigame), &original_minigame,
       "the minigame request");
  hook(natives.start_smithing, reinterpret_cast<void*>(&hooked_smithing), &original_smithing,
       "StartMinigame");
  hook(natives.start_alchemy, reinterpret_cast<void*>(&hooked_alchemy), &original_alchemy,
       "StartAlchemy");
}

void set_local(std::uint32_t local_entity_id, bool online) {
  local_id = local_entity_id;
  in_world = online;
  if (!online) {
    asked.clear();
  }
}

std::vector<wire::CraftReport> take_asks() {
  return std::exchange(to_send, {});
}

void apply_verdict(const wire::CraftVerdict& verdict) {
  for (auto it = asked.begin(); it != asked.end(); ++it) {
    if (it->request != verdict.request) {
      continue;
    }
    const Asked one = *it;
    asked.erase(it);
    if (!verdict.allowed) {
      log_line("CRAFTING the server refused station %u (request %u)", one.station, one.request);
      return;
    }
    if (!entity::by_id(one.station)) {
      return;
    }
    log_line("CRAFTING the server allowed station %u (request %u)", one.station, one.request);
    if (one.component && original_minigame) {
      // The request goes back where the game kept it (its caller dropped it as it failed).
      std::memcpy(one.request_at, one.request_bytes.data(), kRequestBytes);
      original_minigame(one.component, one.request_at);
      return;
    }
    const Start original =
        one.craft == wire::CraftKind::Smithing ? original_smithing : original_alchemy;
    if (original && one.binding) {
      original(one.binding, &own_handle, one.user, one.station);
    }
    return;
  }
}

}  // namespace kcds::runtime::crafting::permits
