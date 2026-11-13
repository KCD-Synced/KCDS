// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "look_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"
#include "native_look.hpp"

namespace kcds::game_client {

DWORD resolve_look_natives() {
  native_look_fade_in = reinterpret_cast<LookFade>(resolve_unique(
      "LookFadeIn",
      "0F 2F 0D ?? ?? ?? 03 76 15 F3 0F 10 05 ?? ?? ?? 03 F3 0F 5E C1 F3 0F 11 81 A0 00 00 00 C3"));
  native_look_fade_out = reinterpret_cast<LookFade>(resolve_unique(
      "LookFadeOut",
      "0F 2F 0D ?? ?? ?? 03 76 15 F3 0F 10 05 ?? ?? ?? 03 F3 0F 5E C1 F3 0F 11 81 A4 00 00 00 C3"));
  character_pose_entry =
      resolve_unique("CharacterSkeletonPose",
                     "48 8D 81 98 06 00 00 C3 CC CC CC CC CC CC CC CC 48 8D 81 18 0F 00 00 C3");
  pose_look_entry =
      resolve_unique("SkeletonPoseLook",
                     "48 8B 81 D0 00 00 00 C3 CC CC CC CC CC CC CC CC 0F B6 81 C8 00 00 00 C3");
  look_state_entry = resolve_unique(
      "PoseBlenderLookState", "88 91 8C 00 00 00 C3 CC 48 89 5C 24 10 57 48 83 EC 30 48 8B 41 38");
  look_target_entry = resolve_unique(
      "PoseBlenderLookTarget",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8D 72 0C 48 8B FA 48 8B E9 "
      "48 8B DA 48 3B D6 74 16 F3 0F 10 03 E8 ?? ?? ?? FF 84 C0 74 1E 48 83 C3 04");
  if (!native_look_fade_in || !native_look_fade_out || !character_pose_entry || !pose_look_entry ||
      !look_state_entry || !look_target_entry) {
    log("Native look signatures missing");
    return 1;
  }
  return 0;
}

DWORD install_look_hooks() {
  if (MH_CreateHook(look_state_entry, reinterpret_cast<void*>(hooked_look_state),
                    reinterpret_cast<void**>(&original_look_state)) != MH_OK ||
      MH_EnableHook(look_state_entry) != MH_OK)
    return 1;
  if (MH_CreateHook(look_target_entry, reinterpret_cast<void*>(hooked_look_target),
                    reinterpret_cast<void**>(&original_look_target)) != MH_OK ||
      MH_EnableHook(look_target_entry) != MH_OK)
    return 1;
  return 0;
}

}  // namespace kcds::game_client
