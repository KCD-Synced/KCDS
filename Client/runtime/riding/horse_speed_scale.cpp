// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "horse_speed_scale.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::runtime::riding::horse_speed_scale {
namespace {

// CAnimatedCharacter::UpdatePhysicalEntityMovement(this, const QuatT& movement): the QuatT is
// the rotation (x, y, z, w) then the frame's translation (x, y, z).
using UpdateMovement = char(__fastcall*)(void* character, float* movement);
// The animated character's entity (IEntity*).
constexpr std::size_t kCharacterEntity = 56;
constexpr std::size_t kTranslation = 4;
constexpr std::size_t kMovementFloats = 7;

UpdateMovement original = nullptr;
std::mutex scales_mutex;
std::unordered_map<void*, float> scales;

float scale_of(void* entity) {
  std::lock_guard<std::mutex> lock(scales_mutex);
  const auto found = scales.find(entity);
  return found == scales.end() ? 1.0f : found->second;
}

char __fastcall hooked(void* character, float* movement) {
  void* entity =
      *reinterpret_cast<void**>(static_cast<std::uint8_t*>(character) + kCharacterEntity);
  const float scale = entity ? scale_of(entity) : 1.0f;
  if (scale == 1.0f || !movement) {
    return original(character, movement);
  }
  // Its own copy: the caller's movement stays as the animation made it.
  float scaled[kMovementFloats];
  for (std::size_t i = 0; i < kMovementFloats; ++i) {
    scaled[i] = movement[i];
  }
  // Across the ground only; falling and climbing stay the physics' own.
  scaled[kTranslation] *= scale;
  scaled[kTranslation + 1] *= scale;
  return original(character, scaled);
}

}  // namespace

bool install(Logger log) {
  void* target = kcds::game_client::resolve_unique(
      "AnimatedCharacterPhysicalMovement",
      "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D A8 18 FF FF "
      "FF 48 81 EC C0 01 00 00 4C 8B 79 38 45 33 ED");
  const bool done = target &&
                    MH_CreateHook(target, reinterpret_cast<void*>(&hooked),
                                  reinterpret_cast<void**>(&original)) == MH_OK &&
                    MH_EnableHook(target) == MH_OK;
  if (log) {
    log("HORSE speed scale %s", done ? "hooked" : "not hooked");
  }
  return done;
}

void set(void* entity, float scale) {
  if (!entity) {
    return;
  }
  std::lock_guard<std::mutex> lock(scales_mutex);
  if (scale < 0 || scale == 1.0f) {
    scales.erase(entity);
  } else {
    scales[entity] = scale;
  }
}

void forget(void* entity) {
  std::lock_guard<std::mutex> lock(scales_mutex);
  scales.erase(entity);
}

}  // namespace kcds::runtime::riding::horse_speed_scale
