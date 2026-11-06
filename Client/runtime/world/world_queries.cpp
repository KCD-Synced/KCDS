#include "world_queries.hpp"

#include "../camera/camera_shot.hpp"
#include "../render/render_view.hpp"
#include "../core/client_internal.hpp"
#include "../core/natives.hpp"
#include "../npcs/native_npcs.hpp"
#include "../physics/world_raycast.hpp"
#include "../replicas/remote.hpp"
#include "../testing/bone_camera.hpp"
#include "../riding/native_horses.hpp"

#include <optional>
#include <vector>

namespace kcds::game_client {

// The network id of the replicated body with this engine id here: the local player, another
// player's replica, a server NPC or a server horse; 0 for none (World.raycast).
std::uint32_t network_id_of_engine(std::uint32_t engine_id) {
  if (engine_id == 0) {
    return 0;
  }
  if (engine_id == local_engine_id.load()) {
    return std::uint32_t(local_network_id.load());
  }
  for (const auto& [id, remote] : remotes) {
    if (remote.engine_id == engine_id) {
      return std::uint32_t(id);
    }
  }
  for (const auto& [id, npc] : native_npcs) {
    if (npc.engine_id == engine_id) {
      return std::uint32_t(id);
    }
  }
  for (const auto& [id, horse] : native_horses) {
    if (horse.engine_id == engine_id) {
      return std::uint32_t(id);
    }
  }
  return 0;
}
// Every replicated body this game shows, with its network id and where its entity stands
// (World.entitiesAround): the local player, other players' replicas, server NPCs and
// server horses.
std::vector<kcd2s::integration::WorldBody> replicated_bodies() {
  std::vector<kcd2s::integration::WorldBody> bodies;
  void* framework = framework_slot ? *framework_slot : nullptr;
  auto actors =
      framework ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304) : nullptr;
  void* entities =
      actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
  if (!entities) {
    return bodies;
  }
  auto add = [&](std::uint64_t network_id, std::uint32_t engine_id) {
    void* entity = engine_id ? get_entity(entities, engine_id) : nullptr;
    const float* tm = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
    if (!tm || network_id == 0) {
      return;
    }
    kcd2s::integration::WorldBody body;
    body.entity = std::uint32_t(network_id);
    body.position[0] = tm[3];
    body.position[1] = tm[7];
    body.position[2] = tm[11];
    bodies.push_back(body);
  };
  add(local_network_id.load(), local_engine_id.load());
  for (const auto& [id, remote] : remotes) {
    add(id, remote.engine_id);
  }
  for (const auto& [id, npc] : native_npcs) {
    add(id, npc.engine_id);
  }
  for (const auto& [id, horse] : native_horses) {
    add(id, horse.engine_id);
  }
  return bodies;
}
// The server's rays and camera shots for this game: traced and filmed on the
// engine thread, answered through the client peer.
void run_server_rays_and_shots(bool in_world) {
  std::vector<kcd2s::integration::WorldRayRequest> rays;
  std::vector<std::optional<kcd2s::integration::CameraShot>> shots;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      rays = client->peer().take_world_rays();
      shots = client->peer().take_camera_orders();
    }
  }
  if (in_world) {
    kcds::runtime::physics::answer_rays(rays);
  }
  // A kit's bone camera (testing/bone_camera.hpp) films after the server's shots.
  if (auto order = bone_camera_order()) {
    shots.push_back(*order);
  }
  kcds::runtime::camera::tick_shots(in_world, local::clock_seconds(), shots);
  // A script's Render.view films after the shots, so its camera is the active view.
  kcds::runtime::render::tick(in_world, local::clock_seconds());
}

}  // namespace kcds::game_client
