#include "equipped_weight.hpp"

#include <cstring>
#include <unordered_set>

#include "../core/game_layouts.hpp"
#include "MinHook.h"

namespace kcds::runtime::inventory::equipped_weight {
namespace {

// The local player's soul (the combat actor's at + 1640); the weight at + 28 of the derived
// values the lambda keeps at + 8.
constexpr std::size_t kActorSoul = kcds::layouts::Actor::kSoul;
constexpr std::size_t kLambdaValues = 8;
constexpr std::size_t kValuesWeight = 28;

using Derive = void(__fastcall*)(void* values, void* soul);
using AddWeight = void*(__fastcall*)(void* lambda, void* item);

Natives natives;
Derive original_derive = nullptr;
AddWeight original_add_weight = nullptr;
std::uint8_t percent = 100;
// While the local soul works its values out: the items it wears.
bool deriving_local = false;
std::unordered_set<void*> worn;

void* local_soul() {
  void* actor = natives.local_actor ? natives.local_actor() : nullptr;
  void* soul = nullptr;
  if (actor) {
    std::memcpy(&soul, static_cast<std::uint8_t*>(actor) + kActorSoul, sizeof(soul));
  }
  return soul;
}

void __fastcall hooked_derive(void* values, void* soul) {
  if (percent >= 100 || deriving_local || !soul || soul != local_soul()) {
    original_derive(values, soul);
    return;
  }
  const std::vector<void*> items =
      natives.worn ? natives.worn(natives.local_actor()) : std::vector<void*>{};
  worn = std::unordered_set<void*>(items.begin(), items.end());
  deriving_local = true;
  original_derive(values, soul);
  deriving_local = false;
  worn.clear();
}

void* __fastcall hooked_add_weight(void* lambda, void* item) {
  if (!deriving_local || !lambda || worn.count(item) == 0) {
    return original_add_weight(lambda, item);
  }
  // A worn item adds only the share of what the game adds.
  auto* values =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(lambda) + kLambdaValues);
  float before = 0.0f;
  std::memcpy(&before, values + kValuesWeight, sizeof(before));
  void* result = original_add_weight(lambda, item);
  float after = 0.0f;
  std::memcpy(&after, values + kValuesWeight, sizeof(after));
  const float scaled = before + (after - before) * (static_cast<float>(percent) / 100.0f);
  std::memcpy(values + kValuesWeight, &scaled, sizeof(scaled));
  return result;
}

}  // namespace

void install(const Natives& given) {
  natives = given;
  const bool done = natives.derive && natives.add_weight && natives.local_actor && natives.worn &&
                    MH_CreateHook(natives.derive, reinterpret_cast<void*>(&hooked_derive),
                                  reinterpret_cast<void**>(&original_derive)) == MH_OK &&
                    MH_CreateHook(natives.add_weight, reinterpret_cast<void*>(&hooked_add_weight),
                                  reinterpret_cast<void**>(&original_add_weight)) == MH_OK &&
                    MH_EnableHook(natives.derive) == MH_OK &&
                    MH_EnableHook(natives.add_weight) == MH_OK;
  if (natives.log) {
    natives.log("INVENTORY equipped weight %s", done ? "hooked" : "not hooked");
  }
}

bool set_percent(std::uint8_t value) {
  const std::uint8_t next = value > 100 ? 100 : value;
  if (next == percent) {
    return false;
  }
  percent = next;
  return true;
}

}  // namespace kcds::runtime::inventory::equipped_weight
