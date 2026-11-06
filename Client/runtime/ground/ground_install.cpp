// Winsock before Windows.h (see client_internal.hpp).
#include "../props/prop_entity.hpp"
#include "../core/client_internal.hpp"

#include "ground_install.hpp"

#include <cstdint>
#include <cstring>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../inventory/kettles.hpp"
#include "../ranged/landings.hpp"
#include "ground_items.hpp"

namespace kcds::game_client {

void install_ground(const kcds::runtime::book::Natives& natives) {
  kcds::runtime::ground::Natives ground;
  ground.services = natives.services;
  ground.item_desc_init = natives.item_desc_init;
  ground.item_desc_free = natives.item_desc_free;
  ground.inventory_add = natives.inventory_add;
  ground.inventory_remove = natives.inventory_remove;
  ground.lay = resolve_unique("GroundLay",
                              "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B "
                              "DA 49 8B F1 48 8B 91 88 00 00 00 49 8B E8 48 8B F9");
  ground.push_function =
      resolve_unique("GroundPushFunction",
                     "48 C7 41 38 00 00 00 00 48 8D 05 ?? ?? ?? ?? 48 89 01 F2 0F 10 02 F2 0F "
                     "11 41 08 8B 42 08 89 41 10 48 8B C1 48 89 49 38");
  ground.item_weight =
      resolve_unique("ItemUnitsWeight",
                     "48 83 EC 38 8B 41 50 0F 29 74 24 20 0F 57 F6 F3 48 0F 2A F0 E8 ?? ?? ?? ?? "
                     "F3 0F 59 C6 0F 28 74 24 20 48 83 C4 38 C3");
  ground.function_free =
      resolve_unique("StdFunctionFree",
                     "40 53 48 83 EC 20 48 8B D9 48 8B 49 38 48 85 C9 74 ?? 48 8B 01 48 3B CB "
                     "0F 95 C2 FF 50 20 48 C7 43 38 00 00 00 00");
  constexpr char kItemLookup[] =
      "48 89 4C 24 20 48 8B 0D ?? ?? ?? ?? 48 81 C1 68 01 00 00 E8 ?? ?? ?? ?? 48 8B F8 48 85 "
      "C0 74 ?? BA 1B 00 00 00";
  ground.item_registry =
      static_cast<void* const*>(resolve_unique("ItemRegistry", kItemLookup, 8, 12));
  ground.item_by_handle = resolve_unique("ItemByHandle", kItemLookup, 20, 24);
  ground.script_on_used =
      resolve_unique("PickableScriptOnUsed",
                     "48 89 5C 24 08 57 48 83 EC 20 49 8B D8 48 8B FA E8 ?? ?? ?? ?? 48 8B C8 48 "
                     "85 C0 75 ?? 48 8B 07 48 8B CF FF 50 58 EB ?? 48 8B 00 8B D3 FF 90 B8 02 00 "
                     "00");
  ground.drop = resolve_unique("OwnerDropUnits",
                               "48 8B C4 48 89 58 20 55 56 57 41 56 41 57 48 8D 68 A9 48 81 EC E0 "
                               "00 00 00 0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8");
  ground.entity_position = [](std::uint32_t id, float* position) {
    void* framework = framework_slot ? *framework_slot : nullptr;
    auto actors = framework
                      ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                      : nullptr;
    void* entities =
        actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
    void* entity = entities && id ? get_entity(entities, id) : nullptr;
    const float* tm = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
    if (!tm) {
      return false;
    }
    position[0] = tm[3];
    position[1] = tm[7];
    position[2] = tm[11];
    return true;
  };
  // A pinned stack's body is taken away, as the game takes a stuck arrow's.
  ground.unphysicalize = [](std::uint32_t id) {
    void* entity = kcds::runtime::props::entity::by_id(id);
    return entity && kcds::runtime::props::entity::physicalize(
                         entity, kcds::runtime::props::entity::Physics::None, 0.0f);
  };
  ground.entity_pose = [](std::uint32_t id, float* position, float* rotation) {
    void* entity = kcds::runtime::props::entity::by_id(id);
    if (!entity) {
      return false;
    }
    kcds::runtime::props::entity::world_pose(entity, position, rotation);
    return true;
  };
  ground.framework = []() -> void* { return framework_slot ? *framework_slot : nullptr; };
  ground.place_in_world = resolve_unique(
      "PlaceItemInWorld", "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 41 8B F8 48 8B 89 98 00 00 00");
  ground.hide_entity = [](std::uint32_t id, bool hidden) {
    kcds::runtime::props::entity::hide(kcds::runtime::props::entity::by_id(id), hidden);
  };
  ground.remove_entity = [](std::uint32_t id) {
    void* framework = framework_slot ? *framework_slot : nullptr;
    auto actors = framework
                      ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                      : nullptr;
    void* entities =
        actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
    if (entities && id) {
      slot<void(__fastcall*)(void*, std::uint32_t, bool)>(entities, 19)(entities, id, false);
    }
  };
  log("GROUND items %s", kcds::runtime::ground::install(ground, log) ? "installed" : "unavailable");
  // Projectiles that landed: the server's stack replaces every client's copy.
  kcds::runtime::ranged::LandingNatives landing;
  landing.entity_transform = [](std::uint32_t id, float* matrix) {
    void* framework = framework_slot ? *framework_slot : nullptr;
    auto actors = framework
                      ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                      : nullptr;
    void* entities =
        actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
    void* entity = entities && id ? get_entity(entities, id) : nullptr;
    const float* tm = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
    if (!tm) {
      return false;
    }
    std::memcpy(matrix, tm, 12 * sizeof(float));
    return true;
  };
  landing.level_item_of = &kcds::runtime::ground::level_item_of;
  // CEntity slot 79 GetPhysics; IPhysicalEntity slot 1 GetType (riding/trample.cpp).
  landing.physics_type = [](std::uint32_t id) {
    void* entity = kcds::runtime::props::entity::by_id(id);
    if (!entity) {
      return -1;
    }
    void* physics = slot<void*(__fastcall*)(void*)>(entity, 79)(entity);
    if (!physics) {
      return 0;
    }
    return slot<int(__fastcall*)(void*)>(physics, 1)(physics);
  };
  // Cooking pots: where their triggers and the local player are.
  kcds::runtime::inventory::KettleNatives kettles;
  kettles.entity_position = ground.entity_position;
  kettles.player_position = [](float* position) {
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    void* entity = actor ? *reinterpret_cast<void**>(actor + 56) : nullptr;
    const float* tm = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
    if (!tm) {
      return false;
    }
    position[0] = tm[3];
    position[1] = tm[7];
    position[2] = tm[11];
    return true;
  };
  kcds::runtime::inventory::install_kettles(kettles);
  landing.remove_entity = ground.remove_entity;
  log("LANDINGS %s",
      kcds::runtime::ranged::install_landings(landing, log) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
