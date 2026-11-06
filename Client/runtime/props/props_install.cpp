// Winsock before Windows.h (see client_internal.hpp).
#include "../input/input_gate.hpp"
#include "../core/client_internal.hpp"

#include "props_install.hpp"

#include <cstdint>

#include "../animation/animation_props.hpp"
#include "../carts/cart_horses.hpp"
#include "../carts/carts_tick.hpp"
#include "../carts/placed_carts.hpp"
#include "../crafting/crafting_install.hpp"
#include "../prefabs/game_prefabs.hpp"
#include "../studio/studio_skin_install.hpp"
#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../inventory/item_types.hpp"
#include "../vfx/placed_effects.hpp"
#include "../vfx/placed_objects_tick.hpp"
#include "ghosts.hpp"
#include "placed_props.hpp"
#include "placer.hpp"
#include "prop_entity.hpp"

namespace kcds::game_client {

kcds::runtime::props::Natives install_props() {
  // Held props of remote players.
  kcds::runtime::props::Natives natives;
  natives.entity_system = []() -> void* {
    void* framework = framework_slot ? *framework_slot : nullptr;
    auto actors = framework
                      ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                      : nullptr;
    return actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
  };
  natives.attach_child = resolve_unique(
      "EntityAttachChild",
      "48 89 5C 24 20 55 56 57 48 8D 6C 24 B9 48 81 EC F0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 "
      "C4 48 89 45 37 49 8B");
  natives.local_entity = []() -> void* {
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    return actor ? *reinterpret_cast<void**>(actor + 56) : nullptr;
  };
  natives.spawn_params_init = resolve_unique(
      "EntitySpawnParams", "48 89 5C 24 08 57 48 83 EC 20 33 FF 48 8D 05 ?? ?? ?? ?? 48 89 41 28");
  natives.physics_component = resolve_unique(
      "EntityPhysicsComponent",
      "48 89 5C 24 10 55 56 57 41 56 41 57 48 8B EC 48 83 EC 40 48 8D 99 A8 00 00 00");
  natives.engine_3d = engine_3d_slot;
  natives.log = log;
  kcds::runtime::props::install(natives);
  log("PROPS attach %s", natives.attach_child ? "resolved" : "unavailable");
  // The props of the players' script animations in their hands.
  kcds::runtime::animation_props::Natives animation_props;
  animation_props.attach_child = natives.attach_child;
  animation_props.item_class = &kcds::runtime::inventory::game_item_class;
  animation_props.log = log;
  animation_props.image = image;
  animation_props.image_size = image_size;
  kcds::runtime::animation_props::install(animation_props);
  return natives;
}

void install_props_and_crafting() {
  const kcds::runtime::props::Natives props = install_props();
  install_crafting(props);
  install_placed_objects(props);
  install_studio_skin();
}

void install_placed_objects(const kcds::runtime::props::Natives& natives) {
  // The props the server places: the mesh's scale and its physical body.
  kcds::runtime::props::entity::PlacementNatives placement;
  placement.set_scale = resolve_unique(
      "EntitySetScale",
      "45 33 C9 4C 8D 51 4C 4C 2B D2 45 8B D8 48 8B C2 F3 42 0F 10 04 10 0F 2E 00 7A 10 75 0E");
  placement.physics_params_init = resolve_unique(
      "PhysicalizeParams",
      "33 C0 C7 41 04 FF FF FF FF 89 01 48 89 41 14 48 89 41 20 89 41 2C 48 89 41 38 48 89 41 40 "
      "48 89 41 48 48 89 41 50 48 89 41 58 48 89 41 60 48 89 41 68");
  kcds::runtime::props::entity::install_placement(placement);
  kcds::runtime::props::placed::install(log, &prop_report, &placed_body_entity,
                                        natives.attach_child);
  log("PROPS placed %s",
      placement.set_scale && placement.physics_params_init ? "installed" : "unavailable");
  // The game's own prefabs a prop can name (a house).
  kcds::runtime::prefabs::install(log);
  // The effects the server places: the game's particle manager and its calls
  // behind the entity binding's LoadParticleEffect.
  kcds::runtime::vfx::placed::Natives effects;
  effects.particle_manager = static_cast<void* const*>(
      resolve_unique("ParticleManager",
                     "4C 8B E0 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 3D ?? ?? ?? ?? 49 8B CC C7 45 "
                     "C7 FF FF FF FF 48 8B 07 48 8B 58 30",
                     15, 19));
  effects.spawn_params_init = resolve_unique(
      "ParticleSpawnParams",
      "40 53 48 83 EC 20 48 8B D9 B8 00 00 80 3F 33 C9 89 0B 66 C7 43 04 00 01 C6 43 06 01");
  effects.load_emitter = resolve_unique(
      "EntityLoadParticleEmitter",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 8B EA 49 8B F9 33 D2 49 8B F0 "
      "48 8B D9 E8");
  effects.attach_child = natives.attach_child;
  effects.body_of = &placed_body_entity;
  effects.log = log;
  log("VFX placed %s", kcds::runtime::vfx::placed::install(effects) ? "installed" : "unavailable");
  // The carts the server places: their parts hang from the root entity, and the
  // local player leads or rides them.
  kcds::runtime::carts::placed::Natives carts;
  carts.attach_child = natives.attach_child;
  carts.local_player = []() -> std::uint32_t { return local_network_id.load(); };
  carts.local_pose = &cart_local_pose;
  carts.place_local = &cart_place_local;
  carts.report = &cart_report;
  carts.press = &press_player_action;
  carts.keep_horse = [](const char* name, const float position[3], const float rotation[4],
                        const float velocity[3], float turn_rate, int side, const char* cart_root,
                        std::uint32_t driver, const float reins_rest[6]) {
    return keep_cart_horse(name, position, rotation, velocity, turn_rate, HarnessSide(side),
                           cart_root, driver, reins_rest);
  };
  log("CARTS reins %s", install_cart_reins() ? "installed" : "unavailable");
  carts.remove_horse = &remove_cart_horse;
  carts.ground_height = &cart_ground_height;
  carts.blocked = &cart_blocked;
  carts.log = log;
  log("CARTS placed %s",
      kcds::runtime::carts::placed::install(carts) ? "installed" : "unavailable");
  // The player's prop previews and placement on the props' natives.
  kcds::runtime::props::ghosts::install(log);
  kcds::runtime::props::placer::install(log);
}

}  // namespace kcds::game_client
