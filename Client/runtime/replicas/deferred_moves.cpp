// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "deferred_moves.hpp"

#include <unordered_map>

namespace kcds::game_client {

namespace {

struct Move {
  core::Vec3 position;
  core::Quat rotation;
};
// Engine thread only: engine id -> where it goes.
std::unordered_map<std::uint32_t, Move> pending;

// The entity system now (CCryAction's actor system at +1304, its entity system at +48, as
// carts/cart_horses.cpp reads them), or null outside a world.
void* entity_system_now() {
  void* framework = framework_slot ? *framework_slot : nullptr;
  if (!framework) {
    return nullptr;
  }
  void* actor_system = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304);
  return actor_system ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor_system) + 48)
                      : nullptr;
}

}  // namespace

void defer_replica_move(std::uint32_t engine_id, const core::Vec3& p, const core::Quat& q) {
  pending[engine_id] = {p, q};
}

void apply_replica_moves() {
  if (pending.empty()) {
    return;
  }
  void* entity_system = entity_system_now();
  if (entity_system) {
    for (const auto& [engine_id, move] : pending) {
      // A body removed since is skipped.
      if (void* entity = get_entity(entity_system, engine_id)) {
        set_transform(entity, move.position, move.rotation, 0);
      }
    }
  }
  pending.clear();
}

}  // namespace kcds::game_client
