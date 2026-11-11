// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "physics_install.hpp"

#include <algorithm>
#include <mutex>

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../gates/gates.hpp"
#include "../level/level_edits.hpp"
#include "../level/story_layers.hpp"
#include "../npcs/native_npcs.hpp"
#include "../npcs/npc_use.hpp"
#include "../physical_props/cloth.hpp"
#include "../terrain/merged_grass.hpp"
#include "../terrain/terrain_edits.hpp"
#include "../vegetation/local_vegetation.hpp"
#include "../world/world_queries.hpp"
#include "line_of_sight.hpp"
#include "local_world.hpp"
#include "world_raycast.hpp"

namespace kcds::game_client {

void install_world_physics() {
  // gEnv->pPhysicalWorld as CryAction's own ray loads it: Rob only with a clear
  // line (physics/line_of_sight.hpp).
  const auto physical_world = static_cast<void* const*>(
      resolve_unique("PhysicalWorld",
                     "48 8B 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 41 B9 04 00 00 00 48 8D 54 "
                     "24 50 48 8B 01 FF 90 18 01 00 00 8B C8 85 C0",
                     3, 7));
  // The terrain shaped while the game runs (World Architect terraforming).
  kcds::runtime::terrain::install(log);
  kcds::runtime::terrain::grass::install(log);
  // Vegetation planted while the game runs (World Architect).
  kcds::runtime::vegetation::install(log);
  // Cloth through the game's soft bodies (physical_props/cloth.hpp).
  kcds::runtime::physical_props::cloth::install(physical_world);
  if (!kcds::runtime::physics::install(physical_world)) {
    log("PHYSICS ray test missing; Rob would be offered through walls");
  }
  // The server's level edits: level entities through the entity system, brushes
  // through their physics (level/level_edits.hpp).
  kcds::runtime::level::Natives level_edits;
  level_edits.physical_world = physical_world;
  level_edits.engine_3d = engine_3d_slot;
  // CVegetation::CalcMatrix: a tree's or bush's world matrix, so a vegetation
  // instance is a level object too.
  level_edits.vegetation_matrix = reinterpret_cast<void(__fastcall*)(void*, float*)>(
      resolve_unique("VegetationCalcMatrix",
                     "48 8B C4 4C 89 40 18 55 53 56 57 48 8D 68 A8 48 81 EC 38 01 00 00 F3 0F 10 "
                     "15 ?? ?? ?? ?? 33 F6 F3 0F 10 0D ?? ?? ?? ?? 48 8B FA 0F 29 70 C8 48 8B D9 "
                     "0F 29 78 B8 44 0F 29 40 A8 44 0F 29 48 98 44 0F 29 50 88 44 0F 29 98 78 FF "
                     "FF FF 44 0F 29 A0 68 FF FF FF 44 0F 29 A8 58 FF FF FF 44 0F 29 B0 48 FF FF "
                     "FF 44 0F 29 B8 38 FF FF FF 0F B6 81 85 00 00 00 48 89 74 24 24 66 44 0F 6E "
                     "C0 0F B6 81 86 00 00 00 45 0F 5B C0 66 0F 6E F0 0F B6 41 7C"));
  // CVegetation::Physicalize: hooked so a tree an edit hides stays without
  // collision when the engine's on-demand physics comes back to its place.
  level_edits.vegetation_physicalize =
      resolve_unique("VegetationPhysicalize",
                     "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 "
                     "8D A8 D8 FE FF FF "
                     "48 81 EC 00 02 00 00 0F 29 70 C8 48 8B F9 0F 29 78 B8 45 33 D2 44 0F 29 40 "
                     "A8 8A DA 44 0F 29 "
                     "48 98 48 63 41 78 48 69 C8 A0 01 00 00 48 8B 05 ?? ?? ?? ?? 4C 8B 40 10 4E "
                     "8B 34 01 4D 85 F6");
  level_edits.entity_system = []() -> void* {
    void* framework = framework_slot ? *framework_slot : nullptr;
    auto actors = framework
                      ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                      : nullptr;
    return actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
  };
  log("LEVEL edits %s",
      kcds::runtime::level::install(level_edits, log) ? "installed" : "unavailable");
  // The level's story layers: gEnv->pEntitySystem as the Engine:LayerSwitch flow node reads it,
  // right before its EnableLayer call (vtable + 592) with the layer's name, on and save.
  auto* entity_system = static_cast<void* const*>(resolve_unique(
      "LayerSwitchEntitySystem",
      "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 39 48 8B 01 44 8A CE 48 8B 54 24 40 44 8A C7 FF 90 50 02 "
      "00 00",
      3, 7));
  log("LEVEL story layers %s",
      kcds::runtime::level::layers::install(entity_system, log) ? "installed" : "unavailable");
  // The server's gates on the props' entity natives and those level edits.
  kcds::runtime::gates::install(log);
  // The server's rays (World.raycast) on the same ray test.
  kcds::runtime::physics::RaycastNatives raycasts;
  raycasts.local_entity = []() -> void* {
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    return actor ? *reinterpret_cast<void**>(actor + 56) : nullptr;
  };
  raycasts.network_id_of = &network_id_of_engine;
  raycasts.replicated_bodies = &replicated_bodies;
  // The 3D engine the HUD found, for the hits' surface names.
  raycasts.engine_3d = engine_3d_slot;
  raycasts.send_reply = [](const kcd2s::integration::WorldRayReply& reply) {
    std::lock_guard<std::mutex> lock(network_mutex);
    return client && client->peer().send_world_ray_reply(reply);
  };
  kcds::runtime::physics::install_raycasts(raycasts);
  // The client scripts' own rays, spheres and camera pose.
  kcds::runtime::physics::LocalWorldNatives local_world;
  local_world.local_entity = raycasts.local_entity;
  local_world.entity_system = level_edits.entity_system;
  local_world.physical_world = physical_world;
  local_world.view_camera = [](float matrix[12], float& fov) {
    if (!view_matrix_valid) {
      return false;
    }
    std::copy(view_matrix.begin(), view_matrix.end(), matrix);
    fov = view_fov;
    return true;
  };
  kcds::runtime::physics::install_local_world(local_world);
  // The use key on a server NPC on the same ray test.
  kcds::runtime::npcs::use::Natives use;
  use.view_camera = [](float matrix[12]) {
    if (!view_matrix_valid) {
      return false;
    }
    std::copy(view_matrix.begin(), view_matrix.end(), matrix);
    return true;
  };
  use.own_physics = []() -> void* {
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    void* entity = actor ? *reinterpret_cast<void**>(actor + 56) : nullptr;
    return entity ? slot<void*(__fastcall*)(void*)>(entity, 79)(entity) : nullptr;
  };
  use.npc_of_engine = [](std::uint32_t engine_id) -> std::uint32_t {
    for (const auto& [id, npc] : native_npcs) {
      if (npc.engine_id == engine_id) {
        return std::uint32_t(id);
      }
    }
    return 0;
  };
  use.send = [](std::uint32_t npc) {
    std::lock_guard<std::mutex> lock(network_mutex);
    return client && client->peer().send_npc_use(npc);
  };
  use.log = log;
  kcds::runtime::npcs::use::install(use);
}

}  // namespace kcds::game_client
