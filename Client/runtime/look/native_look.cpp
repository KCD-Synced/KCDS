#include "native_look.hpp"

#include "../core/client_internal.hpp"
#include "../core/natives.hpp"
#include "../replicas/motion_capture.hpp"
#include "kcd2s/integration/posture.hpp"

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace kcds::game_client {

// MCP: CPoseBlenderLook SetState/+140 and SetTarget/+144.
LookFade native_look_fade_in = nullptr, native_look_fade_out = nullptr;
LookState original_look_state = nullptr;
LookTarget original_look_target = nullptr;
void* look_state_entry = nullptr;
void* look_target_entry = nullptr;
void* character_pose_entry = nullptr;
void* pose_look_entry = nullptr;
std::unordered_map<void*, RemoteLook> remote_looks;  // motion_mutex; scalar owner state only
bool native_pair_alignment(void* movement);
void __fastcall hooked_look_state(void* look, char enabled) {
  {
    std::shared_lock<std::shared_mutex> lock(motion_mutex);
    auto it = remote_looks.find(look);
    if (it != remote_looks.end() && !native_pair_alignment(it->second.movement))
      enabled = it->second.enabled ? 1 : 0;
  }
  original_look_state(look, enabled);
}
std::intptr_t __fastcall hooked_look_target(void* look, const core::Vec3* target) {
  core::Vec3 owned{};
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    auto it = remote_looks.find(look);
    if (it != remote_looks.end() && !native_pair_alignment(it->second.movement)) {
      owned = it->second.target;
      target = &owned;
    }
  }
  return original_look_target(look, target);
}
// Validate every native accessor against the resolved implementation before calling.
// A character's skeleton pose (ICharacterInstance slot 6), null for none or for a pose of another
// class (its look getter, slot 43, is not the game's: live, a character while the world loaded).
void* native_pose(void* character) {
  if (!character || (*reinterpret_cast<void***>(character))[6] != character_pose_entry) {
    return nullptr;
  }
  auto pose = slot<void*(__fastcall*)(void*)>(character, 6)(character);
  if (!pose || (*reinterpret_cast<void***>(pose))[43] != pose_look_entry) {
    return nullptr;
  }
  return pose;
}
void* native_look(void* character) {
  auto pose = native_pose(character);
  if (!pose)
    return nullptr;
  auto look = slot<void*(__fastcall*)(void*)>(pose, 43)(pose);
  if (!look)
    return nullptr;
  auto table = *reinterpret_cast<void***>(look);
  return table[8] == look_state_entry && table[9] == look_target_entry &&
                 table[12] == reinterpret_cast<void*>(native_look_fade_out) &&
                 table[13] == reinterpret_cast<void*>(native_look_fade_in)
             ? look
             : nullptr;
}
bool finite_look(core::Vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::abs(v.x) < 1e6f &&
         std::abs(v.y) < 1e6f && std::abs(v.z) < 1e6f;
}
// What a native look IK works with this frame, for the log. Its CPoseBlenderLook (checked
// by native_look): whether it looks (+ 140), the layer it takes its look poses from (+ 141, set by
// SetLayer), how many look poses its execute found there (+ 220, counts them
// anew every frame and turns nothing without one) and how far it has faded in (+ 424, 0 to 1);
// and the clip on top of that layer of the skeleton.
NativeLookUse native_look_use(void* look, void* skeleton) {
  NativeLookUse use;
  if (!look) {
    return use;
  }
  const auto* blender = static_cast<const std::uint8_t*>(look);
  use.state = blender[140] != 0;
  use.layer = blender[141];
  use.poses = unsigned(std::max<std::int8_t>(0, static_cast<std::int8_t>(blender[220])));
  std::memcpy(&use.fade, blender + 424, sizeof(use.fade));
  if (!skeleton || use.layer >= 16) {
    return use;
  }
  // The skeleton's FIFO of the layer, as capture_existing_animations reads it.
  using FifoCount = unsigned(__fastcall*)(void*, unsigned);
  using FifoEntry = const std::uint8_t*(__fastcall*)(void*, unsigned, unsigned);
  const auto count = slot<FifoCount>(skeleton, 15)(skeleton, use.layer);
  if (count == 0 || count > 16) {
    return use;
  }
  const auto* top = slot<FifoEntry>(skeleton, 18)(skeleton, use.layer, count - 1);
  if (top) {
    use.clip = *reinterpret_cast<const int*>(top + 16);
  }
  return use;
}
// The posture a player's look is logged in.
const char* look_posture_name(bool riding, bool seated, std::int64_t posture_field) {
  if (riding) {
    return "riding";
  }
  if (seated) {
    return "seated";
  }
  kcd2s::integration::PlayerPosture posture;
  if (kcd2s::integration::parse_posture_field(posture_field, posture) && posture.crouched) {
    return "crouched";
  }
  return "standing";
}

}  // namespace kcds::game_client
