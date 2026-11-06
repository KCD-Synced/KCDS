// Where a player aims, up and down included, seen by everyone,
// WHGame 1.5.5.
//
// A drawn bow, a crossbow or a gun points where its owner aims with the game's aim pose blender
// (CPoseBlenderAim, a skeleton pose's, ISkeletonPose slot 41): a state (on or off) and a target in
// the world, which the owner's game sets every frame. The owner's are read from its own blender;
// its replica's blender is held on them, and anything else that sets that blender (the replica's
// own game, which aims nowhere) is overridden.
//
// Engine thread.
#ifndef KCDS_RUNTIME_RANGED_AIM_HPP
#define KCDS_RUNTIME_RANGED_AIM_HPP

#include "kcd2s/core/math.hpp"

namespace kcds::runtime::ranged {

using AimLogger = void (*)(const char* format, ...);

struct AimNatives {
  // CPoseBlenderAim's SetTarget, by which its table is recognised.
  void* set_target = nullptr;
};

// False when a native is missing.
bool install_aim(const AimNatives& natives, AimLogger logger);

// The aim pose blender of a skeleton pose, null for none.
void* aim_of(void* skeleton_pose);

struct AimState {
  kcd2s::core::Vec3 target{};
  bool enabled = false;
};

// What a blender aims at now.
AimState read_aim(void* aim);

// A replica's blender aims where its owner's does, now and whenever its game sets it.
void keep_replica_aim(void* aim, const AimState& owner);

// The blender is no replica's any more (the replica went or its skeleton changed).
void forget_replica_aim(void* aim);

}  // namespace kcds::runtime::ranged

#endif  // KCDS_RUNTIME_RANGED_AIM_HPP
