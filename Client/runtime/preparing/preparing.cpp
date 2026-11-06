#include "preparing.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <string>
#include <utility>

#include "../props/prop_entity.hpp"
#include "MinHook.h"

namespace kcds::runtime::preparing {
namespace {

namespace entity = props::entity;
namespace wire = kcd2s::integration;

// The interrupt node's context: its behaviour's name attribute at + 56 (a CryString
// once read). A node's status at + 8 of its context: 5 done, 6 failed.
constexpr std::size_t kBehaviorName = 56;
constexpr std::size_t kStatus = 8;
constexpr std::uint8_t kFailed = 6;
// How long after a refusal the local player's conversions fail (the station's animation comes
// first: a fire's pan, a smokehouse's sniff).
constexpr ULONGLONG kRefusedMs = 20000;
// How far from the player the station that asked may stand.
constexpr float kStationReach = 6.0f;
constexpr char kStationClass[] = "FoodProcessingTrigger";

using NodeStart = std::int64_t(__fastcall*)(void* node);
using NodeContext = std::uint8_t*(__fastcall*)(void* node);
using StringAttribute = const char* const*(__fastcall*)(void* attribute);

Natives natives;
NodeStart original_interrupt = nullptr;
NodeStart original_prepare = nullptr;
bool online = false;
std::uint32_t next_request = kFirstRequest;
// The last ask, and until when its refusal stands (0: none).
std::uint32_t asked = 0;
ULONGLONG refused_until = 0;
std::vector<wire::CraftReport> asks;

void log_line(const char* format, const char* text, unsigned value) {
  if (natives.log) {
    natives.log(format, text, value);
  }
}

float distance(const float a[3], const float b[3]) {
  const float x = a[0] - b[0];
  const float y = a[1] - b[1];
  const float z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

// The station the player prepares at: the nearest food processing trigger, and whether it smokes
// or dries ("dryer" in its prefab's name; the behaviour's own isSmokehouse property says the
// same).
const char* station_kind(const float player[3], float where[3]) {
  void* best = nullptr;
  float best_distance = kStationReach;
  entity::each_of_class(kStationClass, [&](void* candidate) {
    float there[3];
    entity::world_position(candidate, there);
    const float away = distance(there, player);
    if (away <= best_distance) {
      best = candidate;
      best_distance = away;
    }
  });
  if (!best) {
    std::memcpy(where, player, sizeof(float) * 3);
    return "smoking";
  }
  entity::world_position(best, where);
  const std::string name = entity::name_of(best, 128);
  return name.find("dryer") != std::string::npos ? "drying" : "smoking";
}

// The kind of preparing this interrupt starts, or null for any other interrupt.
const char* preparing_kind(std::uint8_t* context, float where[3]) {
  const auto* text =
      reinterpret_cast<StringAttribute>(natives.string_attribute)(context + kBehaviorName);
  const char* name = text ? *text : nullptr;
  if (!name) {
    return nullptr;
  }
  const bool cooking = std::strcmp(name, "player_cooking") == 0;
  const bool smokehouse = std::strcmp(name, "player_useSmokehouse") == 0;
  if (!cooking && !smokehouse) {
    return nullptr;
  }
  float player[3] = {};
  if (!natives.local_position || !natives.local_position(player)) {
    return nullptr;
  }
  if (cooking) {
    std::memcpy(where, player, sizeof(player));
    return "cooking";
  }
  return station_kind(player, where);
}

std::int64_t __fastcall hooked_interrupt(void* node) {
  auto* context = node ? reinterpret_cast<NodeContext>(natives.node_context)(node) : nullptr;
  float where[3] = {};
  const char* kind = online && context ? preparing_kind(context, where) : nullptr;
  if (kind) {
    asked = next_request++;
    refused_until = 0;
    wire::CraftReport ask;
    ask.kind = wire::CraftReport::Kind::Prepare;
    ask.request = asked;
    ask.station = {where[0], where[1], where[2]};
    ask.reason = kind;
    asks.push_back(ask);
    log_line("PREPARING %s asked (request %u)", kind, asked);
  }
  return original_interrupt(node);
}

std::int64_t __fastcall hooked_prepare(void* node) {
  if (refused_until == 0 || GetTickCount64() >= refused_until) {
    return original_prepare(node);
  }
  // Refused: the game converts nothing and takes nothing.
  auto* context = node ? reinterpret_cast<NodeContext>(natives.node_context)(node) : nullptr;
  if (context) {
    context[kStatus] = kFailed;
  }
  log_line("PREPARING %s (request %u)", "a conversion refused", asked);
  return 0;
}

template <class Fn>
bool hook(void* target, void* detour, Fn* original, const char* name) {
  if (!target || MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    *original = nullptr;
    log_line("PREPARING could not hook %s (%u)", name, 0);
    return false;
  }
  return true;
}

}  // namespace

void install(const Natives& given) {
  natives = given;
  if (!natives.node_context || !natives.string_attribute) {
    log_line("PREPARING %s missing (%u)", "natives", 0);
    return;
  }
  if (!hook(natives.prepare_item, reinterpret_cast<void*>(&hooked_prepare), &original_prepare,
            "C_PrepareItem's start")) {
    return;
  }
  hook(natives.interrupt_node, reinterpret_cast<void*>(&hooked_interrupt), &original_interrupt,
       "the interrupt node");
}

void set_online(bool value) {
  online = value && original_interrupt != nullptr && original_prepare != nullptr;
  if (!online) {
    refused_until = 0;
  }
}

std::vector<wire::CraftReport> take_asks() {
  return std::exchange(asks, {});
}

bool apply_verdict(const wire::CraftVerdict& verdict) {
  if (verdict.request < kFirstRequest) {
    return false;
  }
  if (verdict.request == asked && !verdict.allowed) {
    refused_until = GetTickCount64() + kRefusedMs;
    log_line("PREPARING %s (request %u)", "refused", verdict.request);
  } else if (verdict.request == asked) {
    log_line("PREPARING %s (request %u)", "allowed", verdict.request);
  }
  return true;
}

}  // namespace kcds::runtime::preparing
