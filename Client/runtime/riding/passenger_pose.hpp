// A horse passenger's pose on every game: upright, holding on to the rider (passenger_hold.hpp).
//
// A pose modifier on the pose the body is drawn with (stage 2, pushed by the character's animation
// pass, animation/upper_body.hpp add_pass_listener), as the pointing arm's. The attachments
// (attach/player_attach.hpp) name the bodies every frame while they sit on a horse; a body not
// named again for a moment is let go, and a new one comes into the hold over kBlendIn.
//
// Engine thread, except the modifier, which runs in the game's animation jobs.
#pragma once

#include "passenger_hold.hpp"

namespace kcds::runtime::riding::passenger_pose {

using Logger = void (*)(const char* format, ...);

// Listens to the characters' animation pass.
void install(Logger logger);

// Every frame: this body sits on a horse behind its rider, who is as `rider` says (in the body's
// character space; unknown, the body sits upright).
void hold(void* entity, const passenger_hold::Rider& rider);

// The world went away.
void reset();

}  // namespace kcds::runtime::riding::passenger_pose
