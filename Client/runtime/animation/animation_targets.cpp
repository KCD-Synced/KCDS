// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "animation_targets.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/motion_capture.hpp"
#include "MinHook.h"
#include "animation_capture.hpp"

namespace kcds::game_client {

DWORD resolve_animation_targets(StartupTargets& targets) {
  auto clear_animation = resolve_unique("SkeletonClearLayer",
                                        "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC "
                                        "20 8B DA 33 FF 48 FF C3 48 C1 E3 06 48 03 D9");
  auto stop_animation = resolve_unique("SkeletonStopLayer",
                                       "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 63 F2 83 "
                                       "FE 0F 77 2F 40 0F B6 C6 48 C1 E0 06");
  auto start_animation = resolve_unique(
      "SkeletonStartAnimation",
      "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC C0 00 00 00 48 8B "
      "05 ?? ?? ?? ?? 48 33 C4 48 89 45 17 33 F6 48 89 4D B7 39 35 ?? ?? ?? ?? 49 8B D8 48 89 5D "
      "BF 4C 8B F1 0F 85 ?? ?? ?? ?? 48 8B 89 E8 04 00 00 40 38 B1 A0 0A 00 00");
  human_overlay_natives.getAnimationSet =
      resolve_unique("HumanOverlayAnimationSet", "48 8B 81 70 0A 00 00 48 8B 80 18 02 00 00 C3");
  human_overlay_natives.getSkeleton =
      resolve_unique("HumanOverlayCharacterSkeleton",
                     "48 8D 81 10 01 00 00 C3 CC CC CC CC CC CC CC CC 48 8D 41 24 C3");
  human_overlay_natives.resolveHash =
      resolve_unique("HumanOverlayAlias", "48 83 C1 28 E9 57 FA FF FF");
  human_overlay_natives.entry =
      resolve_unique("HumanOverlayEntry",
                     "48 83 EC 28 4C 8B 41 08 4C 8B C9 41 83 78 FC 00 74 21 85 D2 0F 88 "
                     "?? ?? ?? ?? 41 3B 50 FC");
  human_overlay_natives.characterManager = static_cast<void* const*>(resolve_unique(
      "HumanOverlayCharacterManagerSlot",
      "48 8B 05 ?? ?? ?? ?? 48 05 C8 01 00 00 C3 CC CC 48 8B 81 A0 03 00 00 C3", 3, 7));
  auto facial_pak_changed = resolve_unique(
      "HumanOverlayFacialPakChanged",
      "40 55 56 48 8D 6C 24 98 48 81 EC 68 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 "
      "40 48 8B F1 48 8D 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 44 8B 8E A4 05 08 00 48 8B 01 "
      "44 8B 86 A0 05 08 00 FF 50 60 83 BE A0 05 08 00 00");
  if (!human_overlay_natives.getAnimationSet || !human_overlay_natives.getSkeleton ||
      !human_overlay_natives.resolveHash || !human_overlay_natives.entry ||
      !human_overlay_natives.characterManager || !facial_pak_changed) {
    log("HUMAN OVERLAY native identity signatures missing; adapter not started");
    return 1;
  }
  human_overlay_install_pending.store(true);
  auto motion_param = resolve_unique("SkeletonMotionParam",
                                     "83 FA 11 0F 8D ?? ?? ?? ?? 48 8B C4 48 89 58 08 48 89 68 10 "
                                     "48 89 70 18 48 89 78 20 41 54 41 56 41 57");
  targets.clear_animation = clear_animation;
  targets.stop_animation = stop_animation;
  targets.start_animation = start_animation;
  targets.facial_pak_changed = facial_pak_changed;
  targets.motion_param = motion_param;
  return 0;
}

DWORD install_animation_layer_hooks(const StartupTargets& targets) {
  // The targets under the names initialize_adapter() gave them.
  void* const clear_animation = targets.clear_animation;
  void* const stop_animation = targets.stop_animation;
  if (MH_CreateHook(clear_animation, reinterpret_cast<void*>(hooked_clear_animation),
                    reinterpret_cast<void**>(&original_clear_animation)) != MH_OK)
    return 1;
  if (MH_EnableHook(clear_animation) != MH_OK)
    return 1;
  if (MH_CreateHook(stop_animation, reinterpret_cast<void*>(hooked_stop_animation),
                    reinterpret_cast<void**>(&original_stop_animation)) != MH_OK)
    return 1;
  if (MH_EnableHook(stop_animation) != MH_OK)
    return 1;
  return 0;
}

DWORD install_animation_hooks(const StartupTargets& targets) {
  // The targets under the names initialize_adapter() gave them.
  void* const facial_pak_changed = targets.facial_pak_changed;
  void* const start_animation = targets.start_animation;
  void* const motion_param = targets.motion_param;
  if (MH_CreateHook(facial_pak_changed, reinterpret_cast<void*>(hooked_facial_pak_changed),
                    reinterpret_cast<void**>(&original_facial_pak_changed)) != MH_OK ||
      MH_EnableHook(facial_pak_changed) != MH_OK) {
    log("HUMAN OVERLAY facial pak mutation hook failed; adapter not started");
    return 1;
  }
  if (MH_CreateHook(start_animation, reinterpret_cast<void*>(hooked_start_animation),
                    reinterpret_cast<void**>(&original_start_animation)) != MH_OK)
    return 1;
  if (MH_EnableHook(start_animation) != MH_OK)
    return 1;
  if (MH_CreateHook(motion_param, reinterpret_cast<void*>(hooked_motion_param),
                    reinterpret_cast<void**>(&original_motion_param)) != MH_OK)
    return 1;
  if (MH_EnableHook(motion_param) != MH_OK)
    return 1;
  return 0;
}

}  // namespace kcds::game_client
