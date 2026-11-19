// Hands held at targets (Player.setHandTarget; the NetworkPlayer field "handTargets",
// kcd2s/integration/hand_targets.hpp): every game solves each such player's arm on that player's
// body, the local one and every replica, so the hand reaches the target each frame on top of what
// the body plays, walking and running too (hand_ik.hpp). A target fades in and out over its blend.
// In the owner's own first-person view the target's first-person position and rotation, where
// given, are used instead, so a held item can sit in view without changing what others see.
//
// The solve is a pose modifier on the pose the body is drawn with (stage 2, pushed by the
// character's animation pass, upper_body::add_pass_listener), as pointing.hpp's.
//
// Engine thread, except the modifier, which runs in the game's animation jobs.
#pragma once

#include <string>
#include <vector>

namespace kcds::runtime::animation::hand_targets {

using Logger = void (*)(const char* format, ...);

// Listens to the characters' animation pass.
void install(Logger logger);

// Every frame for a player's body (its engine entity): the player's field ("" for no targets),
// and whether this is the local player's body seen in first person.
void keep(void* entity, const std::string& field, bool first_person, double now);

// Every frame after keep: bodies not kept this frame let go of their targets at once.
void keep_only(const std::vector<void*>& entities);

// The world went away.
void reset();

}  // namespace kcds::runtime::animation::hand_targets
