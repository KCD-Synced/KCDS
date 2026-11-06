#include "ladder_clips.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/motion_capture.hpp"

#include <array>
#include <unordered_map>

namespace kcds::game_client {

// Where each remote player who is on a ladder is, by network id (movement/shared_ladders.hpp).
std::unordered_map<core::EntityId, std::array<float, 3>> ladder_climbers;

// A climb on a ladder: the game's ladder action (C_ActionLadderClimbUpDown) plays the
// fragment LadderClimb, the clip relaxed_climb_ladder on the base layer with the procedural clip
// LayerManualUpdate (CProceduralClipLayerManualUpdate). Its OnEnter
// marks that clip in the layer's FIFO as updated by hand (CA_MANUAL_UPDATE, bit 0 of the clip's
// flags), its Update sets the clip's normalized time every frame from the action's
// parameter LadderFraction (computed in: half a clip per rung, 0 or 0.5 at rest), and
// its OnExit clears the mark. The game's clock leaves a marked clip alone
// while the layer's blends run on. A replica does not run the ladder action (on its own clock the
// clip looped: a climber looked stuck in a looping climb, live): its owner reports the time of a
// clip its game holds so (posture ladder_progress), and the replica's same clip is marked and set
// the same way (C_SkeletonAnim::SetAnimationNormalizedTime, vtable slot 27).
// Stopping the replica's whole base layer instead (its playback scale, which the layer loop
// multiplies into the delta of its blends) froze the blend into the climb too: the
// replica kept the idle it stood in when its owner took the ladder (upright, arms down).
std::unordered_map<core::EntityId, float> ladder_clip_times;
// The clip each replica's base layer holds by hand, by network id.
std::unordered_map<core::EntityId, int> ladder_held_clips;
constexpr std::size_t kSetAnimationNormalizedTime = 27;
// CAnimation (a FIFO entry, 120 bytes): its flags at + 76 (serialized as nFlags).
constexpr std::size_t kAnimationFlags = 76;
constexpr std::uint32_t kAnimationManualUpdate = 1;

std::uint32_t& animation_flags(std::uint8_t* animation) {
  return *reinterpret_cast<std::uint32_t*>(animation + kAnimationFlags);
}

// The clip a skeleton plays last on its base layer (its native entry and id), or nullptr.
std::uint8_t* base_layer_clip(void* skeleton, int& clip) {
  const unsigned count =
      skeleton ? slot<unsigned(__fastcall*)(void*, unsigned)>(skeleton, 15)(skeleton, 0) : 0;
  if (!count || count > 16) {
    return nullptr;
  }
  auto raw = slot<std::uint8_t*(__fastcall*)(void*, unsigned, unsigned)>(skeleton, 18)(skeleton, 0,
                                                                                       count - 1);
  if (raw) {
    clip = *reinterpret_cast<const int*>(raw + 16);
  }
  return raw;
}

// The local player's last base-layer clip when its game holds it by hand (the ladder's climb clip),
// with its normalized time; false while the layer plays on the game's own clock.
bool local_held_climb(int& clip, float& progress) {
  void* skeleton = local_skeleton.load();
  std::uint8_t* raw = base_layer_clip(skeleton, clip);
  if (!raw || !native_animation_progress) {
    return false;
  }
  progress = native_animation_progress(skeleton, raw);
  return (animation_flags(raw) & kAnimationManualUpdate) != 0;
}

// Gives every entry of a clip in a skeleton's base layer back to the game's own clock, as the
// procedural clip's OnExit does once the climb ends.
void release_base_layer_clip(void* skeleton, int clip) {
  const unsigned count =
      skeleton ? slot<unsigned(__fastcall*)(void*, unsigned)>(skeleton, 15)(skeleton, 0) : 0;
  if (count > 16) {
    return;
  }
  using AnimationInFifo = std::uint8_t*(__fastcall*)(void*, unsigned, unsigned);
  for (unsigned index = 0; index < count; ++index) {
    std::uint8_t* raw = slot<AnimationInFifo>(skeleton, 18)(skeleton, 0, index);
    if (raw && *reinterpret_cast<const int*>(raw + 16) == clip) {
      animation_flags(raw) &= ~kAnimationManualUpdate;
    }
  }
}

// owner_clip: the clip the owner's game last started on its base layer, -1 while its replica is
// not animated by its owner's clips.
void hold_ladder_clip(core::EntityId player, void* skeleton, int owner_clip) {
  const auto time = ladder_clip_times.find(player);
  int clip = -1;
  std::uint8_t* raw = base_layer_clip(skeleton, clip);
  const bool hold = time != ladder_clip_times.end() && raw && clip >= 0 && clip == owner_clip;
  const auto held = ladder_held_clips.find(player);
  if (held != ladder_held_clips.end() && (!hold || held->second != clip)) {
    release_base_layer_clip(skeleton, held->second);
    log("LADDER replica of player=%u climb clip=%d back on the game's clock", player, held->second);
    ladder_held_clips.erase(held);
  }
  if (!hold) {
    return;
  }
  // Marked every frame: the FIFO's entries move as finished clips leave it.
  animation_flags(raw) |= kAnimationManualUpdate;
  if (ladder_held_clips.emplace(player, clip).second) {
    log("LADDER replica of player=%u climb clip=%d held by hand at %.3f", player, clip,
        time->second);
  }
  static ULONGLONG next_hold_log = 0;
  if (GetTickCount64() >= next_hold_log) {
    next_hold_log = GetTickCount64() + 1000;
    log("LADDER replica of player=%u climb clip=%d held at %.3f", player, clip, time->second);
  }
  slot<void(__fastcall*)(void*, void*, float, bool)>(skeleton, kSetAnimationNormalizedTime)(
      skeleton, raw, time->second, true);
}

}  // namespace kcds::game_client
