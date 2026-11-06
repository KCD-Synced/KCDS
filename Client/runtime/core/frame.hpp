// One engine frame of the client: the values frame() reads at its start and its
// feature ticks share. Built once per frame on the engine update thread, after the world and the
// local player were found; the ticks read it as frame() read its own locals.
#pragma once

#include <cstdint>

#include "client_internal.hpp"
#include "kcd2s/integration/world_config.hpp"

namespace kcds::game_client {

struct Frame {
  // local::clock_seconds() at the frame's start.
  double now = 0;
  // The local player's network id, and the replicated entities as the mirror held them.
  core::EntityId own = core::kInvalidEntity;
  const MirrorEntities& entities;
  // The server's world.
  const kcd2s::integration::WorldConfig& world;
  // The game's actor and entity systems and its game interface.
  void* actor_system = nullptr;
  void* entity_system = nullptr;
  void* game_interface = nullptr;
  // The local player's actor, entity and entity id.
  void* local_actor = nullptr;
  void* local_entity = nullptr;
  std::uint32_t local_id = 0;
  // The local player's native look (CLookIK) and aim, and what the look read this frame.
  void* local_look = nullptr;
  void* local_aim = nullptr;
  core::Vec3 look_target{};
  core::Vec3 look_tuning{};
  bool look_enabled = false;
  bool look_valid = false;
};

}  // namespace kcds::game_client
