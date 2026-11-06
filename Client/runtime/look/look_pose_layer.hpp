// The owner's look poses on its replica while the game moves the replica itself.
//
// A human's head look is the game's own look IK (CPoseBlenderLook, in WHGame
// 1.5.5): it turns the head toward its target only through the look poses playing on its layer
// (+ 141, CPoseBlenderLook::SetLayer; the look poses found there are counted at + 220
// by its execute, which gives up without them). The human controller plays them on the
// LookPose scope (Animations/Mannequin/ADB/kcd_male_controllerdefs.xml: LookPose layer 15, the
// Looking scope on the same layer without layers of its own), and C_AnimActionLookPose (vtable)
// points the blender at that scope's layer when it enters. The fragment
// picks the posture's look poses (kcd_male_database.adb): standing relaxed_idle_lookposes_head,
// seated dlg_male_neutral_bench_lookposes, a rider lookposes_horserider_idle_head (a player) or
// _upperbody, crouched crouched_idle_lookposes_torso.
//
// A replica plays its owner's clips, the look poses included. While the game itself moves the
// replica, through its rider's native mount or dismount (RiderSync) or the mount it waits for, the
// owner's clips are consumed without playing. The owner's look poses for the new posture start
// within that window (the riding look poses at the very start of the owner's mount) and
// loop until the posture changes again, so once consumed the replica rode on without them until
// its owner's look poses happened to start again (31 s later in the kit logs). The
// look-pose layer therefore waits through the window and plays its owner's latest clip as soon as
// the window is over; every other layer is still consumed.
//
// Portable: no engine pointers, used on the engine thread.
#pragma once

#include <cstdint>

namespace kcds::runtime::look {

// The animation layer of the human LookPose scope.
inline constexpr unsigned kLookPoseLayer = 15;

// What becomes of an owner's clip on one layer of its replica this frame.
enum class OwnerClipStep {
  Done,     // nothing newer than what the replica already took
  Play,     // start it on the replica
  Consume,  // the game moves the replica: take it without playing
  Wait,     // the game moves the replica: keep it for when the game is done
};

// layer: the clip's layer; sequence: the owner's sequence of the clip on that layer (0 for none);
// applied: the last sequence the replica took on it; native_window: the game moves the replica
// itself this frame (its rider's mount or dismount, or the mount it waits for).
inline OwnerClipStep owner_clip_step(unsigned layer, std::uint64_t sequence, std::uint64_t applied,
                                     bool native_window) {
  if (sequence == 0 || sequence <= applied) {
    return OwnerClipStep::Done;
  }
  if (!native_window) {
    return OwnerClipStep::Play;
  }
  if (layer == kLookPoseLayer) {
    return OwnerClipStep::Wait;
  }
  return OwnerClipStep::Consume;
}

}  // namespace kcds::runtime::look
