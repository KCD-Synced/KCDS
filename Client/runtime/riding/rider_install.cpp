// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "rider_install.hpp"

#include "../core/game_layouts.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../inventory/stock_projection.hpp"
#include "MinHook.h"
#include "horse_inspect.hpp"
#include "horse_reservation.hpp"
#include "horse_saddlebags.hpp"
#include "horse_speed_scale.hpp"
#include "horse_tack.hpp"
#include "native_horses.hpp"
#include "player_horse.hpp"
#include "rider_diagnostics.hpp"
#include "seated_body.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace kcds::game_client {

std::atomic<bool> riding_available{true};

namespace {

// Stands in for the game's own check when riding is off: nobody rides.
char __fastcall nobody_mounted(void*) {
  return 0;
}

void turn_riding_off(const char* why) {
  riding_available.store(false);
  log("RIDING off: %s; no horses between players", why);
}

}  // namespace

DWORD resolve_horse_targets(StartupTargets& targets) {
  native_horse_init =
      resolve_unique("HorseInit",
                     "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 83 3D ?? ?? ?? ?? 00 48 8B FA "
                     "48 8B F1 0F 84 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8D 54 24 40 45 33");
  auto mount_target = resolve_unique(
      "PlayerMountHorse",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B B9 68 0D 00 00 32 DB 88 "
      "99 20 0B 00 00 41 8A F0 48 8B EA 48 8B 47 20 48 8B 88 68 06 00 00");
  auto dismount_target = resolve_unique("PlayerDismountHorse",
                                        "48 8B 89 68 0D 00 00 E9 ?? ?? ?? ?? 48 8B C4 48 89 58 08 "
                                        "48 89 70 10 57 48 81 EC C0 00 00 00 80");
  native_human_mounted = reinterpret_cast<HumanMounted>(resolve_unique(
      "HumanMounted",
      "48 83 EC 28 48 8B 89 90 09 00 00 48 8B 01 FF 50 30 3C 02 0F 94 C0 48 83 C4 28 C3"));
  // Human primary v204/v205. Player's wrappers require the LOCAL camera
  // component at +3432, intentionally absent from channel-0 remote C_Player.
  native_remote_horse_mount = reinterpret_cast<HorseMount>(resolve_unique(
      "RemoteHumanMountHorse",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B 89 80 02 00 00 41 8A D8 48 8B FA E8 ?? ?? ?? ?? 45 33 "
      "C9 44 8A C3 48 8B D7 48 8B 08 4C 8B 51 58 48 8B C8 48 8B 5C 24 30 48 83 C4 20 5F 49 FF E2"));
  native_remote_horse_dismount = reinterpret_cast<RemoteDismount>(resolve_unique(
      "RemoteHumanDismountHorse",
      "48 89 5C 24 08 57 48 83 EC 30 48 8B 89 80 02 00 00 41 8A D8 40 8A FA E8 ?? ?? ?? ?? 45 33 "
      "C9 88 5C 24 20 45 33 C0 40 8A D7 48 8B 08 4C 8B 51 60 48 8B C8 41 FF D2"));
  native_rider_ready = reinterpret_cast<RiderReady>(resolve_unique(
      "RiderSyncReady", "48 8B 41 20 80 78 08 02 74 09 80 78 08 0A 74 03 32 C0 C3 B0 01 C3 CC CC"));
  if (!native_human_mounted) {
    native_human_mounted = nobody_mounted;
    turn_riding_off("the game's mounted check is missing");
  }
  if (!native_horse_init || !mount_target || !dismount_target || !native_remote_horse_mount ||
      !native_remote_horse_dismount || !native_rider_ready || std::getenv("KCDS_TEST_RIDING_OFF")) {
    turn_riding_off("a horse native of this game build is missing");
  }
  targets.mount_target = mount_target;
  targets.dismount_target = dismount_target;
  return 0;
}

DWORD resolve_rider_completion_natives() {
  native_rider_action_getter = reinterpret_cast<RiderActionGetter>(
      resolve_unique("NativeRiderActionGetter",
                     "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 20 41 0F B6 E8 48 8B DA "
                     "4C 8B F1 33 FF 49 8B 06 48 8D 54 24 58 44 8B C7 49 8B CE"));
  native_mount_completed = reinterpret_cast<RiderActionEvent>(resolve_unique(
      "NativeMountCompletedEvent",
      "BA 01 00 00 00 E9 32 00 00 00 CC CC 33 D2 38 91 B8 00 00 00 0F 94 C2 E9 80 00 00 00"));
  native_dismount_completed = reinterpret_cast<RiderActionEvent>(resolve_unique(
      "NativeDismountCompletedEvent",
      "E9 03 00 00 00 CC CC CC 48 83 EC 28 80 B9 11 01 00 00 00 75 1D 48 83 B9 E0 00 00 00 00 74"));
  native_dismount_detach = reinterpret_cast<RiderActionEvent>(
      resolve_unique("NativeDismountDetach",
                     "40 53 48 83 EC 20 80 B9 10 01 00 00 00 48 8B D9 75 1E 48 8B 89 F0 00 00 00 "
                     "48 8B 01 FF 50 10 48 8B C8 33 D2 E8 B3 24 22 FE C6 83 10 01 00 00 01"));
  if (!native_rider_action_getter || !native_mount_completed || !native_dismount_completed ||
      !native_dismount_detach) {
    turn_riding_off("a rider completion native is missing");
  }
  return 0;
}

DWORD install_horse_hooks(const StartupTargets& targets) {
  if (!riding_available.load()) {
    return 0;
  }
  // The targets under the names initialize_adapter() gave them.
  void* const mount_target = targets.mount_target;
  void* const dismount_target = targets.dismount_target;
  const bool installed =
      MH_CreateHook(mount_target, reinterpret_cast<void*>(hooked_horse_mount),
                    reinterpret_cast<void**>(&original_horse_mount)) == MH_OK &&
      MH_EnableHook(mount_target) == MH_OK &&
      MH_CreateHook(dismount_target, reinterpret_cast<void*>(hooked_horse_dismount),
                    reinterpret_cast<void**>(&original_horse_dismount)) == MH_OK &&
      MH_EnableHook(dismount_target) == MH_OK;
  if (!installed) {
    turn_riding_off("the mount hooks did not go in");
    return 0;
  }
  // Horse.speedScale; without it horses keep the game's pace.
  kcds::runtime::riding::horse_speed_scale::install(log);
  return 0;
}

DWORD install_seated_riders() {
  if (!riding_available.load()) {
    return 0;
  }
  // A rider stays seated when an unrelated entity near it is removed (riding/seated_body.hpp).
  if (!kcds::runtime::riding::keep_seated_riders(
          resolve_unique("PostureSetBodyPosition",
                         "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B F2 41 8B F8 49 8B D1 48 "
                         "8B D9 E8 ?? ?? ?? ?? 3B B3 80 00 00 00"),
          log)) {
    turn_riding_off("the seated body hook is missing");
  }
  return 0;
}

void install_body_position_hooks() {
  void* set = resolve_unique("BodyPositionSet",
                             "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 40 "
                             "48 8B F2");
  void* stand = resolve_unique("BodyPositionStand", "83 FA 01 0F 84 ?? ?? ?? ?? 48 89 5C 24 08");
  const bool installed =
      set && stand &&
      MH_CreateHook(set, reinterpret_cast<void*>(hooked_body_position_set),
                    reinterpret_cast<void**>(&original_body_position_set)) == MH_OK &&
      MH_EnableHook(set) == MH_OK &&
      MH_CreateHook(stand, reinterpret_cast<void*>(hooked_body_position_stand),
                    reinterpret_cast<void**>(&original_body_position_stand)) == MH_OK &&
      MH_EnableHook(stand) == MH_OK;
  log("RIDER body position hooks %s", installed ? "installed" : "missing");
}

DWORD install_horse_reservations() {
  if (!riding_available.load()) {
    return 0;
  }
  // A far rider's horse release must not crash (riding/horse_reservation.hpp).
  constexpr const char* kReleaseHorse =
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 4C 8B C2 48 8B FA 48 8D 54 24 30 48 8B F1 "
      "E8 ?? ?? ?? ?? 48 8B D7 48 8B 18 E8 ?? ?? ?? ?? 48 85 C0 74 0A 33 D2 48 8B C8 E8 ?? ?? ?? "
      "?? 48 8B 0D ?? ?? ?? ?? 48 8B D7 48 8B 01 FF 50 18 48 8B 08";
  kcds::runtime::riding::HorseReservationNatives horses;
  horses.reserve = resolve_unique(
      "ReserveHorse",
      "48 89 5C 24 10 57 48 83 EC 20 48 8B DA C6 44 24 30 00 48 8B F9 E8 ?? ?? ?? ?? 48 85 C0 "
      "74 0A B2 01 48 8B C8 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B D3 48 8B 01 FF 50 18 48 "
      "8B 08 48 8B 91 38 01 00 00");
  horses.release = resolve_unique("ReleaseHorse", kReleaseHorse);
  horses.registry = static_cast<void**>(resolve_unique("ActorRegistry", kReleaseHorse, 63, 67));
  if (!kcds::runtime::riding::guard_horse_reservations(horses, log)) {
    turn_riding_off("the horse reservation hooks are missing");
  }
  return 0;
}

void install_player_horses() {
  // The horse inspect screen's comparison with the player's horse (riding/horse_inspect.hpp):
  // the screen's opening and the player's horse lookup.
  kcds::runtime::riding::horse_inspect::Natives inspect;
  inspect.open = resolve_unique(
      "HorseInspectOpen",
      "48 89 5C 24 08 48 89 74 24 10 55 57 41 56 48 8B EC 48 83 EC 70 8B FA 4C 8B F1 E8 ?? ?? ?? "
      "?? 84 C0 74 07 32 C0 E9 ?? ?? ?? ?? 48 8D 15");
  inspect.front = resolve_unique(
      "ContainerFront",
      "0F B6 C2 4C 8B C9 48 8D 04 80 48 83 7C C1 68 00 75 03 33 C0 C3 4C 8B 44 C1 60 48 8B 4C C1 "
      "58");
  kcds::runtime::riding::horse_inspect::install(inspect, log);

  // A horse the server gives the local player is its own horse in its game
  // (riding/player_horse.hpp): the game's player-horse setter and the companion getter
  // it reads back.
  kcds::runtime::riding::PlayerHorseNatives natives;
  natives.set_player_horse = resolve_unique(
      "SetPlayerHorse",
      "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 20 8B DA 4C 8B "
      "F1 E8 ?? ?? ?? ?? 4C 8B 80 60 01 00 00 49 8B 88 A8 00 00 00 48 8B 01 FF 90 C8 01 00 00 "
      "49 8B 4E 20 33 D2");
  natives.companion_of = resolve_unique(
      "ActorCompanion",
      "40 53 48 83 EC 20 48 8B 89 68 06 00 00 8A DA 48 8B 01 FF 90 B0 03 00 00 48 8B C8 8A D3 "
      "E8 ?? ?? ?? ?? 48 85 C0 74 17 48 8B 08 33 D2 4C 8B 81 00 04 00 00 48 8B C8 48 83 C4 20 "
      "5B 49 FF E0");
  player_horses_installed = kcds::runtime::riding::init_player_horses(natives, log);
  if (!player_horses_installed) {
    log("HORSE player horse signatures missing; given horses stay stolen ones for their owners");
  }

  // A server horse's saddlebags held by the server (riding/horse_saddlebags.hpp): the
  // game's move of an item into another inventory, and a horse's body by its network
  // id (the actor system's entity system, + 48, as the carts' horses find theirs).
  kcds::runtime::riding::saddlebags::Natives bags;
  bags.move_item = resolve_unique(
      "InventoryMoveItem",
      "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 81 EC F0 00 00 00 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 84 24 E0 00 00 00 4C 8B F9 44 88 4C 24 30 33 C9 41 8B E8");
  bags.horse_entity = [](std::uint32_t horse) -> void* {
    constexpr std::size_t kFrameworkActorSystem = 1304;
    constexpr std::size_t kActorSystemEntities = 48;
    const auto found = native_horses.find(core::EntityId(horse));
    void* framework = framework_slot ? *framework_slot : nullptr;
    if (found == native_horses.end() || !framework) {
      return nullptr;
    }
    void* actors =
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + kFrameworkActorSystem);
    void* entities =
        actors
            ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + kActorSystemEntities)
            : nullptr;
    return entities ? get_entity(entities, found->second.engine_id) : nullptr;
  };
  bags.horse_actor = [](std::uint32_t horse) -> void* {
    const auto found = native_horses.find(core::EntityId(horse));
    return found == native_horses.end() ? nullptr : found->second.actor;
  };
  // A take or put goes out at once, as a container's (riding/horse_saddlebags.hpp).
  bags.send = [](bool take, std::uint32_t stash, const std::string& row,
                 std::uint32_t units) -> bool {
    kcd2s::integration::InventoryReport report;
    report.kind = take ? kcd2s::integration::InventoryReport::Kind::StashTake
                       : kcd2s::integration::InventoryReport::Kind::StashPut;
    report.request = stash;
    report.row = row;
    report.units = units;
    std::lock_guard<std::mutex> lock(network_mutex);
    return client && client->peer().send_inventory_report(report);
  };
  log("SADDLEBAGS %s",
      kcds::runtime::riding::saddlebags::install(bags, log) ? "installed" : "unavailable");

  // A player's own tack changes in the inventory screen's Horse tab (riding/horse_tack.hpp, issue):
  // the soul equipment manager's equip and unequip (the equipment interface's slots 40 and
  // 41 are thunks to them) and the screen's handler of goInHorseTab / goOutHorseTab.
  kcds::runtime::riding::horse_tack::Natives tack;
  tack.equip = resolve_unique("EquipmentManagerEquip",
                              "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 "
                              "8B D9 41");
  tack.unequip = resolve_unique("EquipmentManagerUnequip",
                                "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 "
                                "8B F2 48 8B F9 48");
  tack.horse_tab =
      resolve_unique("InventoryHorseTabMessage",
                     "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 "
                     "8B F9 4C 8B F2 48");
  tack.horse_of_manager = [](void* manager) -> std::uint32_t {
    constexpr std::size_t kActorSoul = kcds::layouts::Actor::kSoul;
    constexpr std::size_t kSoulEquipment = kcds::layouts::Soul::kEquipment;
    constexpr std::size_t kEquipmentManager = 288;
    if (!manager) {
      return 0;
    }
    for (const auto& [id, horse] : native_horses) {
      if (!horse.actor) {
        continue;
      }
      auto* soul =
          *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(horse.actor) + kActorSoul);
      if (soul && *reinterpret_cast<void**>(soul + kSoulEquipment + kEquipmentManager) == manager) {
        return std::uint32_t(id);
      }
    }
    return 0;
  };
  tack.menu_open = [] { return game_menu_open(); };
  tack.row_of = [](std::uint64_t handle, std::string& row) {
    std::uint64_t revision = 0;
    return kcds::runtime::inventory::row_of(handle, row, revision);
  };
  tack.bags_row_of = [](std::uint32_t horse, std::uint64_t handle, std::string& row) {
    return kcds::runtime::riding::saddlebags::row_of(horse, handle, row);
  };
  tack.send_equip = [](std::uint32_t horse, const std::string& row, bool from_bags) -> bool {
    kcd2s::integration::InventoryReport report;
    report.kind = kcd2s::integration::InventoryReport::Kind::HorseGearEquip;
    report.request = horse;
    report.row = row;
    report.used = from_bags;
    std::lock_guard<std::mutex> lock(network_mutex);
    return client && client->peer().send_inventory_report(report);
  };
  tack.send_remove = [](std::uint32_t horse, const std::array<std::uint8_t, 16>& item_class) {
    kcd2s::integration::InventoryReport report;
    report.kind = kcd2s::integration::InventoryReport::Kind::HorseGearRemove;
    report.request = horse;
    std::memcpy(report.item.data(), item_class.data(), item_class.size());
    std::lock_guard<std::mutex> lock(network_mutex);
    return client && client->peer().send_inventory_report(report);
  };
  log("HORSE TACK %s",
      kcds::runtime::riding::horse_tack::install(tack, log) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
