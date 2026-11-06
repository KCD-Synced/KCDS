// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "appearance_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include <cstdint>
#include <cstring>

#include "components.hpp"
#include "disguise.hpp"
#include "first_person_head.hpp"
#include "grime.hpp"
#include "outfit.hpp"
#include "worn_only.hpp"
#include "player_skeleton.hpp"
#include "script_gender.hpp"
#include "unisex_clothing.hpp"

namespace kcds::game_client {

void install_outfit(const kcds::runtime::book::Natives& natives) {
  kcds::runtime::appearance::outfit::Natives outfit;
  outfit.services = natives.services;
  outfit.inventory_find = natives.inventory_find;
  outfit.item_desc_init = natives.item_desc_init;
  outfit.item_desc_free = natives.item_desc_free;
  outfit.inventory_add = natives.inventory_add;
  outfit.inventory_remove = natives.inventory_remove;
  // An item's health set as the game sets it (clamped to its quality's band).
  outfit.item_health_set =
      resolve_unique("ItemHealthSet",
                     "48 89 5C 24 08 57 48 83 EC 40 F3 0F 10 41 54 49 8B F8 0F 29 74 24 30 48 "
                     "8B D9 0F 29 7C 24 20 0F 28 F9 0F 2E C7");
  kcds::runtime::appearance::outfit::install(outfit, log);
  // Player.wearItem's pieces on the local player.
  kcds::runtime::appearance::worn_only::install(log);
  {
    // Dirt, blood and wear of replicas.
    kcds::runtime::appearance::grime::Natives grime;
    grime.item_state = resolve_unique(
        "ItemArmorState",
        "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B C8 48 8B 10 4C 8B 42 18 BA 04 00 00 00 "
        "41 FF D0");
    // ArmorRuntimeData's load marks a full change: lea rdx, key; ...; call notice.
    if (auto* load = static_cast<std::uint8_t*>(
            resolve_unique("ArmorStateLoaded",
                           "83 8F B0 00 00 00 03 48 8D 15 ?? ?? ?? ?? 48 8B CF C7 87 B4 00 00 "
                           "00 01 00 00 00 E8"))) {
      std::int32_t key = 0;
      std::int32_t call = 0;
      std::memcpy(&key, load + 10, sizeof(key));
      std::memcpy(&call, load + 28, sizeof(call));
      grime.full_state = load + 14 + key;
      grime.changed = load + 32 + call;
    }
    log("APPEARANCE grime %s",
        kcds::runtime::appearance::grime::install(grime, log) ? "installed" : "unavailable");
  }
}

void install_appearance_guards(const kcds::runtime::book::Natives& natives) {
  const bool visor_guard = kcds::runtime::appearance::outfit::install_visor_guard(resolve_unique(
      "PlayerVisorUpdate", "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 54 24 38"));
  log("APPEARANCE visor guard %s", visor_guard ? "installed" : "unavailable");
  const bool preset_guard = kcds::runtime::appearance::outfit::install_preset_guard(
      resolve_unique("ClothingPresetDue",
                     "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 "
                     "20 41 56 48 83 EC 20 48 8B F1 BB 01 00 00 00"));
  log("APPEARANCE preset guard %s", preset_guard ? "installed" : "unavailable");
  const bool unisex = kcds::runtime::appearance::unisex_clothing::install(
      resolve_unique(
          "ClothingWearCheck",
          "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 01 48 8B F2 "
          "48 8B D9 FF 90 E0 01 00 00"),
      resolve_unique(
          "ClothingAddComponent",
          "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 55 41 56 41 57 48 "
          "83 EC 60 4C 8B E9"));
  log("APPEARANCE unisex clothing %s", unisex ? "installed" : "unavailable");
  kcds::runtime::appearance::components::Natives components;
  components.set_component = resolve_unique(
      "SetCharacterComponent",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 60 48 8B D9 41 8B F9");
  components.string_init = natives.string_init;
  components.string_free = natives.string_free;
  components.string_assign = natives.string_assign;
  kcds::runtime::appearance::components::install(components, log);
  // The view switch, which turns the player's own head's FirstPersonView group.
  kcds::runtime::appearance::first_person_head::install(resolve_unique(
      "FirstPersonViewSwitch",
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 41 8A F0 40 8A FA 48 8B D9 E8 ?? ?? ?? ?? 84 "
      "C0 74 17 E8"));
  // A woman's local body on her own skeleton: CEntity::LoadCharacter.
  log("APPEARANCE player skeleton swap %s",
      kcds::runtime::appearance::player_skeleton::install(
          resolve_unique("EntityLoadCharacter",
                         "40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 A0 48 81 EC 60 01 00 00 48 "
                         "8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 50 44 8B FA 45 8B E1 33 D2 4D 8B F0"),
          log)
          ? "installed"
          : "unavailable");
  kcds::runtime::appearance::disguise::install(
      reinterpret_cast<kcds::runtime::appearance::disguise::ComponentLookup>(
          resolve_unique("EntityComponent", "4C 8B 81 A8 00 00 00 45 33 D2 49 8B C8")),
      log);
  // A woman player may use alchemy tables, the grindstone, horses, bodies and pockets.
  kcds::runtime::appearance::script_gender::Natives script_gender;
  script_gender.get_gender = resolve_unique(
      "SoulScriptGetGender",
      "40 53 48 83 EC 40 48 8B CA 48 8B DA E8 ?? ?? ?? ?? 4C 8B 03 48 85 C0 74 40 48 8B 80 B8 0C "
      "00 00 48 8D 54 24 20 C7 44 24 20 04 00 00 00 0F B6 48 1C");
  script_gender.handler_soul = resolve_unique(
      "ScriptHandlerSoul",
      "48 89 5C 24 08 57 48 83 EC 20 48 8D 54 24 38 48 C7 44 24 38 00 00 00 00 E8 ?? ?? ?? ?? 48 "
      "8B 5C 24 38 84 C0 74");
  log("APPEARANCE script gender %s",
      kcds::runtime::appearance::script_gender::install(script_gender, log) ? "installed"
                                                                            : "unavailable");
}

}  // namespace kcds::game_client
