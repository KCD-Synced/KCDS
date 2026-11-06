#include "replica_removal.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "removal_guard.hpp"
#include "character_retirement.hpp"

#include <cstdint>
#include <cstring>

namespace kcds::game_client {

// Removing a remote C_Player replica (created on channel 0) runs the engine's client player UI
// unbind: live diagnostics showed the local C_Player+3272 (I_UIPlayer, a C_UIPlayer) and the
// C_UIPlayer+16 player back-reference both cleared, so the local player lost native hit feedback.
// The GUI module owns that C_UIPlayer, so it outlives the removal. The binding is
// captured before the removal and restored through the original C_Player::SetUIPlayer (vtable slot
// 286), which also runs the original UI setup.
// Root cause of the lost local UI binding: removing a channel-0 remote C_Player dispatches the
// GUI module's client player removal event, whose handler unbinds the UI player
// from the manager's current client player (manager+144) and then clears it. A second removal then
// called the unbind with a null player and crashed. While this adapter removes a
// remote replica on the engine thread, that handler is skipped.
// The engine also removes the previous session's player replicas itself when a new world loads
// after the player left to the main menu: the unbind then ran with no client player bound and
// crashed the same way (leave, rejoin,with rdx 0).
// With no client player bound there is nothing to unbind, so the handler is skipped then too.
constexpr std::size_t kBoundClientPlayer = 144;
ClientPlayerUnbind original_client_player_unbind = nullptr;
bool removing_remote_replica = false;
void* __fastcall hooked_client_player_unbind(void* manager) {
  if (removing_remote_replica) {
    log("UI PLAYER unbind skipped for remote replica removal manager=%p", manager);
    return nullptr;
  }
  void* bound = nullptr;
  std::memcpy(&bound, static_cast<std::uint8_t*>(manager) + kBoundClientPlayer, sizeof(bound));
  if (!bound) {
    log("UI PLAYER unbind skipped: no client player bound manager=%p", manager);
    return nullptr;
  }
  return original_client_player_unbind(manager);
}
// Removes a replica's entity (CEntitySystem::RemoveEntity, slot 19, confirmed). While it
// goes, the game's combat does not queue new animations into its action controller
// (replicas/removal_guard.hpp). The number of animations left out.
unsigned remove_replica_entity(void* entity_system, std::uint32_t engine_id, void* actor,
                               core::EntityId player) {
  engine_stage = "remote replica removal";
  record_character_retirement(entity_system, engine_id);
  kcds::runtime::replicas::begin_replica_removal(actor, unsigned(player));
  slot<void(__fastcall*)(void*, std::uint32_t, bool)>(entity_system, 19)(entity_system, engine_id,
                                                                         false);
  return kcds::runtime::replicas::end_replica_removal();
}
// The C_Player constructor builds a C_MissileWeaponPlayerController at +3440 whose
// constructor registers its listener interface (+8, so player+3448) in the UI state
// registry at services+216 (slot 5). The C_Player destructor and
// the controller destructor never unregister it; the only removal (slot 7)
// matches by name and would drop the local player's listener too. After a remote replica
// was removed, the next UI state change (closing the inventory, then opening the menu:
// -> ->) called the stale listener and crashed in
// on the freed player's null entity. The registry keeps its entries in a vector at
// +352 (begin +8, end +16); each 32-byte entry holds the name, a 128-bit
// state mask at +8 and the listener at +24. Clearing the removed replica's mask means the dispatch
// (tests the mask) never calls it again; the entry itself stays allocated.
// The same UI state registry keeps its active states as a bit set at +8 and the valid states at
// +16 (sets a state,tests it). Live logs on 1.5.5: state 11 is set
// exactly while the full-screen game UI (inventory, map, journal) is open and cleared on close.
constexpr unsigned kFullScreenUiState = 11;
bool local_full_screen_ui() {
  if (!get_services)
    return false;
  auto registry =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(get_services()) + 216);
  if (!registry)
    return false;
  const auto active = *reinterpret_cast<const std::uint64_t*>(registry + 8);
  const auto valid = *reinterpret_cast<const std::uint64_t*>(registry + 16);
  const auto bit = std::uint64_t{1} << kFullScreenUiState;
  return (valid & bit) && (active & bit);
}
void silence_remote_ui_state_listener(void* remote_actor, void* local_actor) {
  if (!remote_actor || !local_actor || !get_services)
    return;
  auto registry =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(get_services()) + 216);
  if (!registry)
    return;
  auto begin = *reinterpret_cast<std::uint8_t***>(registry + 352 + 8);
  auto end = *reinterpret_cast<std::uint8_t***>(registry + 352 + 16);
  if (!begin || end < begin || end - begin > 4096) {
    log("UI STATE listener registry rejected registry=%p", registry);
    return;
  }
  const auto remote_listener = static_cast<std::uint8_t*>(remote_actor) + 3448;
  const auto local_listener = static_cast<std::uint8_t*>(local_actor) + 3448;
  std::uint8_t* remote_entry = nullptr;
  bool local_found = false;
  for (auto it = begin; it != end; ++it) {
    auto entry = *it;
    if (!entry)
      continue;
    auto listener = *reinterpret_cast<std::uint8_t**>(entry + 24);
    if (listener == local_listener)
      local_found = true;
    if (listener == remote_listener)
      remote_entry = entry;
  }
  // New replicas never register this local-only listener. Keep the old exact-pointer guard
  // for a listener supplied by another native construction path.
  if (!remote_entry) {
    return;
  }
  if (!local_found) {
    log("UI STATE listener not silenced remote=%p local_found=%d remote_found=%d", remote_actor,
        int(local_found), int(remote_entry != nullptr));
    return;
  }
  std::memset(remote_entry + 8, 0, 16);
  log("UI STATE listener silenced remote=%p entry=%p", remote_actor, remote_entry);
}
LocalUiBinding capture_local_ui(void* local_actor) {
  LocalUiBinding binding;
  if (!local_actor)
    return binding;
  auto ui = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 3272);
  if (!ui)
    return binding;
  binding.actor = local_actor;
  binding.ui = ui;
  binding.ui_vtable = *reinterpret_cast<const void**>(ui);
  return binding;
}
void restore_local_ui(const LocalUiBinding& binding, void* local_actor) {
  if (!binding.ui || binding.actor != local_actor)
    return;
  auto actor = static_cast<std::uint8_t*>(local_actor);
  if (*reinterpret_cast<void**>(actor + 3272))
    return;
  auto ui = static_cast<std::uint8_t*>(binding.ui);
  auto back_reference = reinterpret_cast<void**>(ui + 16);
  if (*reinterpret_cast<const void**>(ui) != binding.ui_vtable || *back_reference) {
    log("UI PLAYER rebind refused: ui=%p no longer matches the captured unbound object", ui);
    return;
  }
  // C_Player::SetUIPlayer: sub rsp,28h; mov [rcx+0CC8h],rdx. Fail closed on any other target.
  static constexpr std::uint8_t kSetUiPlayerPrologue[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89,
                                                          0x91, 0xC8, 0x0C, 0x00, 0x00};
  auto set_ui_player = slot<void(__fastcall*)(void*, void*)>(local_actor, 286);
  if (std::memcmp(reinterpret_cast<const void*>(set_ui_player), kSetUiPlayerPrologue,
                  sizeof(kSetUiPlayerPrologue)) != 0) {
    log("UI PLAYER rebind refused: C_Player slot 286 is not SetUIPlayer");
    return;
  }
  *back_reference = local_actor;
  set_ui_player(local_actor, binding.ui);
  log("UI PLAYER rebound local=%p ui=%p after remote removal", local_actor, binding.ui);
}

}  // namespace kcds::game_client
