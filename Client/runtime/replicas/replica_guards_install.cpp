// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "replica_guards_install.hpp"

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../input/input_mode.hpp"
#include "MinHook.h"
#include "held_items.hpp"
#include "lod_guard.hpp"
#include "removal_guard.hpp"

namespace kcds::game_client {

void install_player_state_guard() {
  // Optional: without them a remote player's handler state can still lock local input.
  auto player_state_push = resolve_unique(
      "PlayerStateNoninteractivePush",
      "40 53 48 83 EC 20 66 83 B9 CE 02 00 00 00 48 8B D9 75 2C E8 ?? ?? ?? ?? 48 8B 48 08 48 8B "
      "01 FF 90 E0 00 00 00 48 8B 15 ?? ?? ?? ?? 41 B0 01");
  auto player_state_pop = resolve_unique(
      "PlayerStateNoninteractivePop",
      "48 83 EC 28 B8 FF FF 00 00 66 01 81 CE 02 00 00 75 2C E8 ?? ?? ?? ?? 48 8B 48 08 48 8B 01 "
      "FF 90 E0 00 00 00 48 8B 15 ?? ?? ?? ?? 45 33 C0");
  if (player_state_push && player_state_pop &&
      MH_CreateHook(player_state_push, reinterpret_cast<void*>(hooked_player_state_push),
                    reinterpret_cast<void**>(&original_player_state_push)) == MH_OK &&
      MH_CreateHook(player_state_pop, reinterpret_cast<void*>(hooked_player_state_pop),
                    reinterpret_cast<void**>(&original_player_state_pop)) == MH_OK &&
      MH_EnableHook(player_state_push) == MH_OK && MH_EnableHook(player_state_pop) == MH_OK) {
    log("CONTROL replica state handler guard installed");
  } else {
    log("CONTROL replica state handler guard unavailable");
  }
}

void install_replica_guards() {
  // What replicas hold in a hand is drawn with their skeletons.
  log("REPLICA held items %s", install_held_items() ? "installed" : "unavailable");
  // Actors without an AI LOD entry (player replicas) are left out of its update.
  log("REPLICA LOD guard %s",
      kcds::runtime::replicas::install_lod_guard(
          resolve_unique("LodUpdate",
                         "48 89 5C 24 10 55 56 57 48 83 EC 20 41 8A E8 48 8B F2 48 8B "
                         "D9 E8 ?? ?? ?? ?? 48 8B F8 48 89 44 24 40 48 8B CF 48 8B 00"),
          log)
          ? "installed"
          : "unavailable");
  // A replica removed while it fights: no combat animation is queued into its
  // action controller while its entity goes.
  log("REPLICA removal guard %s",
      kcds::runtime::replicas::install_removal_guard(
          resolve_unique("QueueCombatAnimation",
                         "40 53 48 83 EC 30 48 8B 01 48 8B DA 0F 29 74 24 20 0F 28 F2 48 8B 88 "
                         "F8 01 00 00 48 8B 01 FF 90 D0 02 00 00 48 8B 08 4C 8B 81 30 01 00 00 "
                         "48 8B C8 41 FF D0 48 8B C8 48 85 C0 74 1B 48 8B 13 83 7A 28 00 74 06 "
                         "83 7A 28 04 75 0C 48 8B 00 0F 28 D6 FF 90 98 00 00 00 48 8B 0B 48 85 "
                         "C9 74 05 E8"),
          image, log)
          ? "installed"
          : "unavailable");
}

}  // namespace kcds::game_client
