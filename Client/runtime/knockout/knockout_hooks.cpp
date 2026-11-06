#include "knockout_hooks.hpp"

#include "game_over.hpp"

#include "../camera/photo_mode.hpp"
#include "../camera/third_person.hpp"
#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../input/input_gate.hpp"
#include "MinHook.h"
#include "knockout.hpp"
#include "replica_faint.hpp"

namespace kcds::game_client {

kcds::runtime::knockout::Natives resolve_knockout_natives() {
  kcds::runtime::knockout::Natives knockout_natives;
  // The script binding's call of the buff manager getter.
  knockout_natives.buff_manager =
      resolve_unique("BuffManager",
                     "48 8B 4D B0 48 8D 55 D0 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F 28 45 D0 "
                     "4C 8D 45 C0 0F 28 4D E0",
                     14, 18);
  knockout_natives.set_state =
      resolve_unique("SoulSetState",
                     "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 70 0F 29 70 E8 48 8B F1 0F 29 "
                     "78 D8 44 0F 29 40 C8 F3 44 0F 10 05 ?? ?? ?? ?? 8B DA 85 D2 75");
  knockout_natives.console_slot = console_slot;
  native_soul_is_unconscious = reinterpret_cast<kcds::runtime::knockout::IsUnconscious>(
      resolve_unique("SoulIsUnconscious",
                     "48 83 EC 38 0F 29 74 24 20 0F 57 F6 0F 2F B1 08 07 00 00 73 13 45 33 C0 "
                     "41 8D 50 37 E8 ?? ?? ?? ?? 0F 2E C6 7A 02 74 0C"));
  // the original actor-state wake request retires the fallen action
  // through its native action manager and queues the authored GetUp successor.
  native_request_replica_wake = reinterpret_cast<RequestReplicaWake>(resolve_unique(
      "ReplicaNativeWake",
      "40 53 48 83 EC 20 48 8B 81 88 00 00 00 48 8B D9 48 8B 88 90 09 00 00 48 8B 01 FF 90 "
      "90 01 00 00 84 C0 74 2A 48 8B CB E8"));
  // The game's singleton getter as the knock-out screen's lambda calls it.
  knockout_natives.game =
      resolve_unique("GameSingleton",
                     "48 89 5C 24 40 E8 ?? ?? ?? ?? 48 8B 48 18 48 8B 01 FF 90 18 01 00 00 48 8B "
                     "08 48 8B 51 38 48 8B C8 FF D2 B2 AF",
                     6, 10);
  // gEnv->pRenderer as the flow node Image:ScreenFader loads it.
  knockout_natives.renderer = static_cast<void**>(resolve_unique(
      "Renderer", "48 8B 0D ?? ?? ?? ?? 4C 8D 44 24 30 0F 10 47 7C 41 B1 01 48 8D 15", 3, 7));
  // The unconscious buff instance's vtable, as its factory sets it (the base
  // constructor, the vtable, + 216 the time and reason zeroed, + 224 -1, + 232 active), which
  // tells the exhaustion faint from a knock-out (knockout/faint.hpp).
  knockout_natives.unconscious_vtable = resolve_by_vtable_class(
      "UnconsciousBuffInstance",
      "48 8B CB E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 C7 83 D8 00 00 00 00 00 00 00 48 89 03 "
      "48 C7 83 E0 00 00 00 FF FF FF FF 66 C7 83 E8 00 00 00 01 00",
      11, 15, ".?AVC_UnconsciousSoulBuffInstance@rpgmodule@wh@@", true);
  // The game's photo mode (F1) allowed or not by the server (Freecam.setEnabled), and its
  // close without the character's remark ("HRAC_VYPNUL_PHOTOMODE").
  void* left_photo_mode = resolve_unique(
      "LeftPhotoModeRemark",
      "48 89 4C 24 08 53 48 83 EC 30 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 80 00 00 00 48 8B 08 "
      "48 8B 91 00 02 00 00 48 8B C8 FF D2 48 8B D8 48 8B 48 38");
  log("PHOTOMODE gate %s",
      kcds::runtime::camera::install_photo_mode(
          reinterpret_cast<void* (*)()>(knockout_natives.game), left_photo_mode, log)
          ? "installed"
          : "unavailable");
  // The game's third-person camera on F5, under the server's ThirdPerson mode, and
  // the camera manager's view update that places it out of the world.
  void* view_update = resolve_unique(
      "CameraManagerViewUpdate",
      "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 00 01 00 00 F3 0F 10 81 68 04 00 00 48 8B FA "
      "0F 2F 05 ?? ?? ?? ?? 48 8B D9 77 11 E8");
  log("CAMERA third person %s", kcds::runtime::camera::third_person::install(
                                    reinterpret_cast<void* (*)()>(get_services), view_update, log)
                                    ? "installed"
                                    : "unavailable");
  // That camera hears the mouse itself: held while the UI owns it.
  void* camera_input = resolve_unique(
      "ThirdPersonCameraInput",
      "48 8B C4 48 89 58 08 48 89 78 10 55 48 8B EC 48 83 EC 70 80 3A 01 48 8B FA 0F 29 70 E8 "
      "48 8B D9 0F 29 78 D8 0F 85 BD 01 00 00 83 7A 04 08");
  kcds::runtime::camera::third_person::install_mouse_gate(camera_input, camera_mouse_blocked);
  return knockout_natives;
}

bool install_knockout(const kcds::runtime::knockout::Natives& knockout_natives) {
  if (!native_soul_is_unconscious || !native_request_replica_wake ||
      !kcds::runtime::knockout::install(knockout_natives, log)) {
    log("KNOCKOUT natives missing; players would die in a server world");
    return false;
  }
  // the push of an actor's ragdoll action (see hooked_push_ragdoll_action).
  void* push_ragdoll_action =
      resolve_unique("PushRagdollAction",
                     "48 89 5C 24 18 57 48 83 EC 20 48 8B F9 48 8D 54 24 30 48 8B 49 08 41 B0 "
                     "08 48 8B 49 50 E8");
  if (!push_ragdoll_action ||
      MH_CreateHook(push_ragdoll_action, reinterpret_cast<void*>(hooked_push_ragdoll_action),
                    reinterpret_cast<void**>(&original_push_ragdoll_action)) != MH_OK ||
      MH_EnableHook(push_ragdoll_action) != MH_OK) {
    log("KNOCKOUT ragdoll action native missing; knocked-out replicas would fight their pose");
    return false;
  }
  // the start of every game over, refused in a server world (game_over.hpp).
  void* game_over_start =
      resolve_unique("GameOverStart", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 60 83");
  const bool game_over_hooked = kcds::runtime::knockout::game_over::install(
      game_over_start, []() { return multiplayer_world_ready.load(); }, log);
  log("KNOCKOUT game over %s",
      game_over_hooked ? "refused in server worlds" : "native missing; a server world could end");
  // the exhausted buff's faint check, from its entry to the player-flag test
  // (soul + 0xCF0 bit 0x10, see knockout/replica_faint.hpp).
  void* exhausted_check = resolve_unique(
      "ExhaustedFaintCheck",
      "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 B9 48 81 EC B0 00 00 00 48 8B 05 "
      "?? ?? ?? ?? 48 33 C4 48 89 45 37 48 8B F9 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C8 E8 ?? "
      "?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 48 8B 9F 88 00 00 00 48 85 DB 0F 84 ?? ?? ?? ?? 8B 83 "
      "F0 0C 00 00 C1 E8 04 A8 01");
  log("KNOCKOUT replica faint gate %s",
      kcds::runtime::knockout::replica_faint::install(exhausted_check, is_replica_soul, log)
          ? "installed"
          : "unavailable (a replica's own game could make it faint)");
  return true;
}

}  // namespace kcds::game_client
