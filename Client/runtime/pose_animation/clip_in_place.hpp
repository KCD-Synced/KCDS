// A body kept where it stands under a cutscene's clip: a clip of the game's own taken
// by its path (Player.playAnimation's clip, Animations/....caf, as the studio's export plays the
// executioner's whipping) carries its scene's walking in the skeleton, so the body stood up to
// 0.9 m off the player's own place, which did not move (in the studio and on a
// server). Every client keeps each body that plays one, the local player's and the others',
// where its player stands: the hips' way across the ground from the body's place, smoothed over
// kDriftSeconds (the walking goes, a strike's sway stays), is taken back by moving the hips the
// other way in their parent's space (joint_turns::set_shift, the joint operator queue's
// PushPosition). Nothing moves the player: every client draws the same body in the same place.
// The studio keeps its own character in place itself (studio/studio_game_clip.hpp).
//
// Engine thread.
#pragma once

#include <cstdint>

namespace kcds::runtime::pose_animation::clip_in_place {

// A body whose clip layer changed (a replica's clip, from replicas/replica_tick.cpp): watched
// while it plays a game clip by path. engine_id: its entity's.
void watch(std::uint32_t engine_id);

// Every frame in a world (local_entity: the player's own body, always watched).
void tick(void* local_entity);

// Out of a world: every body let go.
void reset();

}  // namespace kcds::runtime::pose_animation::clip_in_place
