// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "body_collisions.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::game_client {

namespace {

// The collision event holds the two entities' ids here.
constexpr std::size_t kFirstEntityId = 168;
constexpr std::size_t kSecondEntityId = 172;
// The C_Actor's entity id, and its combat object's C_Actor.
constexpr std::size_t kActorEntityId = 48;
constexpr std::size_t kMovementActor = 504;

using ActorCollision = std::int64_t(__fastcall*)(void* collider, const std::uint8_t* event);

ActorCollision original_actor_collision = nullptr;

bool replica_body(std::uint32_t entity_id) {
  std::lock_guard<std::mutex> lock(combat_identity_mutex);
  for (const auto& [soul, movement] : remote_combat_movements) {
    (void)soul;
    const auto* actor = *reinterpret_cast<const std::uint8_t* const*>(
        static_cast<const std::uint8_t*>(movement) + kMovementActor);
    if (actor && *reinterpret_cast<const std::uint32_t*>(actor + kActorEntityId) == entity_id) {
      return true;
    }
  }
  return false;
}

bool local_body(std::uint32_t entity_id) {
  const auto* local = static_cast<const std::uint8_t*>(local_combat_actor.load());
  return local && *reinterpret_cast<const std::uint32_t*>(local + kActorEntityId) == entity_id;
}

std::int64_t __fastcall hooked_actor_collision(void* collider, const std::uint8_t* event) {
  const auto first = *reinterpret_cast<const std::uint32_t*>(event + kFirstEntityId);
  const auto second = *reinterpret_cast<const std::uint32_t*>(event + kSecondEntityId);
  if (first != second && (local_body(first) || replica_body(first)) &&
      (local_body(second) || replica_body(second))) {
    return 0;
  }
  return original_actor_collision(collider, event);
}

}  // namespace

bool install_body_collisions() {
  void* actor_collision = resolve_unique(
      "ActorCollision",
      "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 A0 FE FF FF 48 81 EC 60 02 00 "
      "00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 50 01 00 00 4C 8B EA 48 8B F1 E8 ?? ?? ?? ?? 83 B8 "
      "98 03 00 00 00 0F 84 D8 04 00 00");
  if (!actor_collision) {
    log("CHARACTERS body collisions: the function was not found, bodies keep hurting each other");
    return false;
  }
  const bool hooked =
      MH_CreateHook(actor_collision, reinterpret_cast<void*>(hooked_actor_collision),
                    reinterpret_cast<void**>(&original_actor_collision)) == MH_OK &&
      MH_EnableHook(actor_collision) == MH_OK;
  log("CHARACTERS body collisions %s", hooked ? "installed" : "failed");
  return hooked;
}

}  // namespace kcds::game_client
