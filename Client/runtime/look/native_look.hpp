// The game's native look IK (CPoseBlenderLook) as the client reads and drives it: the look
// natives checked at startup, the hooks that keep a replica's look on its owner's, and the reads
// for the look logs.
#pragma once

#include <cstdint>
#include <unordered_map>

#include "kcd2s/core/math.hpp"

namespace kcds::game_client {

namespace core = kcd2s::core;

// CPoseBlenderLook SetState/+140 and SetTarget/+144 (hooked: the originals) and its fades.
using LookState = void(__fastcall*)(void*, char);
using LookTarget = std::intptr_t(__fastcall*)(void*, const core::Vec3*);
using LookFade = void(__fastcall*)(void*, float);
extern LookFade native_look_fade_in;
extern LookFade native_look_fade_out;
extern LookState original_look_state;
extern LookTarget original_look_target;
// The resolved entries the look accessors are checked against.
extern void* look_state_entry;
extern void* look_target_entry;
extern void* character_pose_entry;
extern void* pose_look_entry;

// A replica's look as its owner set it, by the replica's look (under motion_mutex).
struct RemoteLook {
  void* movement{};
  core::Vec3 target{};
  bool enabled{};
};
extern std::unordered_map<void*, RemoteLook> remote_looks;
bool native_pair_alignment(void* movement);
void __fastcall hooked_look_state(void* look, char enabled);
std::intptr_t __fastcall hooked_look_target(void* look, const core::Vec3* target);

// A character's skeleton pose and its look IK, null when they are not the game's.
void* native_pose(void* character);
void* native_look(void* character);
bool finite_look(core::Vec3 v);

// What a native look IK works with this frame, for the log.
struct NativeLookUse {
  bool state = false;
  unsigned layer = 0;
  unsigned poses = 0;
  float fade = 0;
  int clip = -1;
};
NativeLookUse native_look_use(void* look, void* skeleton);
// The posture a player's look is logged in.
const char* look_posture_name(bool riding, bool seated, std::int64_t posture_field);

}  // namespace kcds::game_client
