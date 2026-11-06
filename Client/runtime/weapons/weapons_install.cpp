// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "weapons_install.hpp"

#include <cstdint>
#include <cstring>

#include "../appearance/outfit.hpp"
#include "../carry/body_carry_tick.hpp"
#include "../carry/carry_items.hpp"
#include "../carry/carry_orders.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../ground/ground_items.hpp"
#include "../ranged/shots.hpp"
#include "drawn_weapons.hpp"

namespace kcds::game_client {

void install_weapons() {
  // Weapons in the hands: what a player draws, its replica draws.
  kcds::runtime::weapons::Natives natives;
  natives.draw_item = resolve_unique("WeaponDrawItem", "48 83 EC 28 4C 8B D9 48 89 54 24 38");
  natives.create_item = &kcds::runtime::appearance::outfit::create_item_in_inventory;
  natives.find_item = resolve_unique("ItemByWuid", "4C 8B 02 4C 8B C9 49 8B C0 48 C1 E8 38 3C 02");
  // `mov rcx, [rip + disp32]` of the item manager at + 12 of the holster.
  if (auto holster = static_cast<std::uint8_t*>(
          resolve_unique("WeaponHolsterItem",
                         "48 83 EC 28 4C 8B D1 48 89 54 24 38 48 8B 0D ?? ?? ?? ?? 48 8D 54 "
                         "24 38 48 81 C1 68 01 00 00 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B "
                         "D0 49 8B CA E8 ?? ?? ?? ?? B0 01"))) {
    natives.holster_item = holster;
    std::int32_t displacement;
    std::memcpy(&displacement, holster + 15, 4);
    natives.item_manager = reinterpret_cast<void* const*>(holster + 19 + displacement);
  }
  const bool weapons = kcds::runtime::weapons::install(natives, log);
  log("WEAPONS %s", weapons ? "installed" : "unavailable");
  // Shots with bows and crossbows: every client fires the same projectile.
  kcds::runtime::ranged::Natives shots;
  shots.fire = resolve_unique("FireProjectile",
                              "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 48 81 "
                              "EC B0 00 00 00 0F 29 70 D8");
  shots.launch = resolve_unique("ProjectileLaunch",
                                "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 "
                                "8D 68 D8 48 81 EC 20 01 00 00 0F 29 70 E8 48 8B D9");
  shots.projectile_owners = resolve_unique(
      "ProjectileOwners",
      "40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 90 48 85 DB 75 ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? "
      "?? 48 8B 1D ?? ?? ?? ?? 48 85 DB 75 ?? 39 1D ?? ?? ?? ?? 75 ?? E8 ?? ?? ?? ?? 48 8B 05 ?? "
      "?? ?? ?? 48 8D 54 24 30 45 33 C0 41 8D 48 01 FF ?? 8B 54 24 30 48 8B D8 F0 01 15 ?? ?? ?? "
      "?? F0 FF 05 ?? ?? ?? ?? F0 FF 05 ?? ?? ?? ?? 90 48 89 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? "
      "FF 15 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3 CC 48 89 5C 24 18 57");
  shots.shooting_expansion = resolve_unique(
      "ShootingExpansion", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 01 B2 04");
  shots.create_item = natives.create_item;
  shots.item_in_hand = &kcds::runtime::weapons::item_in_hand;
  shots.local_actor = [] { return local_combat_actor.load(); };
  shots.missile_weapon =
      resolve_unique("MissileWeaponOfItem",
                     "40 53 48 83 EC 20 BA 02 00 00 00 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 12 "
                     "48 8B CB E8 ?? ?? ?? ?? 48 8B 43 58");
  shots.set_loaded = resolve_unique("MissileWeaponSetLoaded",
                                    "40 53 48 83 EC 20 48 8B D9 38 91 D8 00 00 00 0F 85");
  shots.released =
      resolve_unique("ShotReleased",
                     "40 53 48 83 EC 20 48 8B D9 48 8B 49 40 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? "
                     "?? 48 8B 4B 40 E8 ?? ?? ?? ?? 33 D2 48 8B C8 E8 ?? ?? ?? ?? C6 43 5A");
  shots.load_ammunition = resolve_unique(
      "BowLoadAmmunition",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B 8A 80 02 00 00 48 8B DA E8 ?? ?? ?? ?? 48 8B 78 40 48 "
      "8B CF E8 ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ?? 4C 39 80 E0 00 00 00 75 23");
  shots.unload_ammunition = resolve_unique(
      "BowUnloadAmmunition",
      "40 53 48 83 EC 20 48 8B 81 E0 00 00 00 48 8D 54 24 30 48 8B D9 48 89 44 24 30 48 8B 0D "
      "?? ?? ?? ?? 48 81 C1 68 01 00 00 E8 ?? ?? ?? ?? 48 85 C0 74 0E 48 8B C8 E8 ?? ?? ?? ?? FF "
      "88 C0 00 00 00");
  log("SHOTS %s", kcds::runtime::ranged::install(shots, log) ? "installed" : "unavailable");
  // Items carried in the arms, with the game's carry action; items found as the weapons
  // find theirs.
  kcds::runtime::carry::Natives carry;
  carry.create_action =
      resolve_unique("CarryActionCreate",
                     "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 83 3D ?? ?? ?? ?? 00 49 8B "
                     "F8 48 8B 59 08 48 8B F2 75 ?? E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8D "
                     "54 24 30 45 33 C0 B9 D8 00 00 00");
  carry.put_down = resolve_unique("CarryActionPutDown",
                                  "48 89 5C 24 18 55 56 57 48 83 EC 30 48 8B D9 48 8B 49 70");
  carry.let_go = resolve_unique("CarryActionLetGo", "40 53 48 83 EC 20 80 B9 90 00 00 00 05");
  carry.find_item = natives.find_item;
  carry.item_manager = natives.item_manager;
  carry.lay = &lay_carried_item;
  carry.take_away = &kcds::runtime::ground::take_away;
  carry.lies_in_world = &kcds::runtime::ground::lies_in_world;
  carry.item_position = &carried_item_position;
  carry.report_put_down = &report_carry_put_down;
  log("CARRY %s", kcds::runtime::carry::install(carry, log) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
