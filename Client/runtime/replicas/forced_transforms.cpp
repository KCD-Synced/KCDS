#include "forced_transforms.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../props/prop_physics.hpp"
#include "../riding/native_horses.hpp"

#include <vector>

namespace kcds::game_client {

// Applies server-authored teleports (FORCE_XFORM) on the engine update thread. Commands are drained
// under the network mutex, which is released before any engine call. Draining adopts the forced
// epoch for owned entities, so the caller must apply before it captures and sends owner input.
// Only the local player is written here: other entities receive the forced position through the
// replicated position field in their snapshots.
// WHGame 1.5.5 script SetWorldPos copies the entity world Matrix34, replaces the
// translation entries 3/7/11 and calls SetWorldTM (entity vtable slot 30) with reason
// 0x400000. The same call keeps rotation, scale and parent semantics of the original engine path.
bool apply_forced_transforms(void* entity_system, void* local_entity, std::uint32_t local_id,
                             core::EntityId own, std::uint32_t revision) {
  auto pending = [&] {
    std::lock_guard<std::mutex> lock(network_mutex);
    return client->peer().take_forced_transforms();
  }();
  bool applied = false;
  for (const auto& command : pending) {
    if (command.revision != revision) {
      log("FORCE_XFORM dropped entity=%u revision=%u current=%u", command.entity, command.revision,
          revision);
      continue;
    }
    void* target = nullptr;
    const char* kind = "player";
    if (command.entity == own) {
      if (get_entity(entity_system, local_id) != local_entity) {
        log("FORCE_XFORM dropped: local entity 0x%x no longer resolves", local_id);
        continue;
      }
      target = local_entity;
    } else {
      // The client that simulates a horse moves its native horse, so its motion reports carry the
      // forced transform. Every other client follows the replicated snapshot.
      auto horse = native_horses.find(command.entity);
      if (horse == native_horses.end() || !horse->second.authority) {
        log("FORCE_XFORM entity=%u applied through replicated snapshot", command.entity);
        continue;
      }
      target = get_entity(entity_system, horse->second.engine_id);
      kind = "horse";
      if (!target) {
        log("FORCE_XFORM dropped: horse %u has no native entity", command.entity);
        continue;
      }
    }
    engine_stage = "forced SetWorldTM";
    const float* current = slot<const float*(__fastcall*)(void*)>(target, 32)(target);
    if (!current) {
      log("FORCE_XFORM dropped: %s entity has no world matrix", kind);
      continue;
    }
    if (command.has_rotation) {
      // WHGame 1.5.5 script SetWorldAngles builds a pure rotation matrix from the
      // quaternion with this formula, keeps translation entries 3/7/11 and calls SetWorldTM with
      // reason 0.
      const auto& q = command.rotation;
      const float x2 = q.x + q.x;
      const float y2 = q.y + q.y;
      const float z2 = q.z + q.z;
      float matrix[12];
      matrix[0] = 1.0f - q.y * y2 - q.z * z2;
      matrix[1] = q.x * y2 - q.w * z2;
      matrix[2] = q.x * z2 + q.w * y2;
      matrix[3] = current[3];
      matrix[4] = q.w * z2 + q.x * y2;
      matrix[5] = 1.0f - q.x * x2 - q.z * z2;
      matrix[6] = q.y * z2 - q.w * x2;
      matrix[7] = current[7];
      matrix[8] = q.x * z2 - q.w * y2;
      matrix[9] = q.y * z2 + q.w * x2;
      matrix[10] = 1.0f - q.x * x2 - q.y * y2;
      matrix[11] = current[11];
      constexpr unsigned kScriptSetWorldAnglesReason = 0;
      slot<void(__fastcall*)(void*, const float*, unsigned)>(target, 30)(
          target, matrix, kScriptSetWorldAnglesReason);
      applied = true;
      log("FORCE_ROT applied %s=%u epoch=%u rot=(%.3f %.3f %.3f %.3f)", kind, command.entity,
          unsigned(command.epoch), q.x, q.y, q.z, q.w);
    }
    if (command.has_position) {
      float matrix[12];
      std::memcpy(matrix, current, sizeof(matrix));
      matrix[3] = command.position.x;
      matrix[7] = command.position.y;
      matrix[11] = command.position.z;
      slot<void(__fastcall*)(void*, const float*, unsigned)>(target, 30)(target, matrix,
                                                                         kScriptSetWorldPosReason);
      // The body keeps no speed from where it was (after Player.spawn on a player in
      // the world the first step flung it tens of metres a second, actual=(23 -38 3), and the fall
      // hurt it). The living body's velocity is set to none, as a prop's is stopped.
      if (command.entity == own) {
        void* body = kcds::runtime::props::physics::body_of(target);
        const bool stopped = body && kcds::runtime::props::physics::set_velocity(body, {}, {});
        log("FORCE_XFORM local body speed cleared=%u", unsigned(stopped));
      }
      applied = true;
      log("FORCE_XFORM applied %s=%u epoch=%u pos=(%.2f %.2f %.2f)", kind, command.entity,
          unsigned(command.epoch), command.position.x, command.position.y, command.position.z);
    }
  }
  return applied;
}
}  // namespace kcds::game_client
