#pragma once

// Player.playPairedAnimation: where the slave's body starts. The game's paired clips
// are made around one origin, each starting at its own place there (the clip's start location,
// exported with it: the master behind the victim, the victim in front, facing away). The game's
// animation set gives it, IAnimationSet slot 20 GetAnimationDCCWorldSpaceLocation(id, QuatT&)
// (the clip's global header's start location at + 128, only when its flags have 0x800,
// else false and the identity). Both bodies start at the master's place; the slave goes from
// there back to the origin by the master clip's start location and out by its own:
// slave = master * inverse(master start) * slave start. The turn kept is the heading alone, so
// the body stands upright.
//
// Engine thread (start_location); the rest is plain math.
namespace kcds::runtime::animation::paired_start {

// A place: a turn (quaternion x, y, z, w) and a position, laid out as the engine's QuatT.
struct Place {
  float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float t[3] = {0.0f, 0.0f, 0.0f};
};

// a * b: the place b in a's frame.
Place compose(const Place& a, const Place& b);

// The place that undoes a.
Place inverse(const Place& a);

// The turn about the up axis (z) alone, the position kept.
Place heading_only(const Place& a);

// Where the slave's body starts: the master's place, back to the clips' origin by the master
// clip's start location, then out by the slave clip's; upright.
Place slave_place(const Place& master, const Place& master_start, const Place& slave_start);

// A clip's start location in its scene; false when the clip has none.
bool start_location(void* animation_set, int clip, Place& place);

}  // namespace kcds::runtime::animation::paired_start
