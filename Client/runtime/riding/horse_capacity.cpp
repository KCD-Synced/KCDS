#include "horse_capacity.hpp"

#include <unordered_map>

#include "MinHook.h"

namespace kcds::runtime::riding::horse_capacity {
namespace {

// The soul's inventory capacity among its derived stats.
constexpr int kInventoryCapacity = 23;

using DerivedStat = float(__fastcall*)(void* soul, int id, void* context);

DerivedStat original = nullptr;
std::unordered_map<void*, float> limits;
// A server horse's soul -> its derived stats a breed or resource set, by id.
std::unordered_map<void*, std::unordered_map<int, float>> overrides;

float __fastcall hooked(void* soul, int id, void* context) {
  const auto stats = overrides.find(soul);
  if (stats != overrides.end()) {
    const auto stat = stats->second.find(id);
    if (stat != stats->second.end()) {
      return stat->second;
    }
  }
  if (id == kInventoryCapacity) {
    const auto found = limits.find(soul);
    if (found != limits.end()) {
      return found->second;
    }
  }
  return original(soul, id, context);
}

}  // namespace

void install(void* derived_stat, Logger log) {
  const bool done = derived_stat &&
                    MH_CreateHook(derived_stat, reinterpret_cast<void*>(&hooked),
                                  reinterpret_cast<void**>(&original)) == MH_OK &&
                    MH_EnableHook(derived_stat) == MH_OK;
  if (log) {
    log("HORSE carry limits %s", done ? "hooked" : "not hooked");
  }
}

void set(void* soul, float weight) {
  if (!soul) {
    return;
  }
  if (weight < 0.0f) {
    limits.erase(soul);
  } else {
    limits[soul] = weight;
  }
}

void forget(void* soul) {
  limits.erase(soul);
  overrides.erase(soul);
}

void set_speed(void* soul, float speed) {
  // rpg_param RunSpeedBaseGuiMinHorse and RunSpeedBaseGuiMaxHorse (WHGame 1.5.5 tables).
  constexpr float kGuiMin = 0.847f;
  constexpr float kGuiMax = 0.951f;
  if (speed < 0.0f) {
    set_stat(soul, kNormalizedRunSpeedBase, -1.0f);
    set_stat(soul, kRunSpeedBase, -1.0f);
    return;
  }
  // The inspect screen shows a horse's speed as 30 + 30 x (value - min) / (max - min), clamped
  // (for the speed properties); the value stands for that.
  constexpr float kScreenLow = 30.0f;
  constexpr float kScreenHigh = 60.0f;
  const float clamped =
      speed < kScreenLow ? kScreenLow : (speed > kScreenHigh ? kScreenHigh : speed);
  const float value =
      kGuiMin + (kGuiMax - kGuiMin) * (clamped - kScreenLow) / (kScreenHigh - kScreenLow);
  set_stat(soul, kNormalizedRunSpeedBase, value);
  set_stat(soul, kRunSpeedBase, value);
}

void set_stat(void* soul, int id, float value) {
  if (!soul) {
    return;
  }
  if (value < 0.0f) {
    const auto found = overrides.find(soul);
    if (found != overrides.end()) {
      found->second.erase(id);
    }
  } else {
    overrides[soul][id] = value;
  }
}

}  // namespace kcds::runtime::riding::horse_capacity
