// The players' pose animations (Player.setPoseAnimation): the NetworkPlayer field
// poseAnimation (kcd2s/integration/pose_animation.hpp) played on the player's body, the local
// one and every replica, from when this client got it: each frame the keys are blended at the
// time played (pose_turns_at) and the joints the body has by name are turned
// (pose_animation/joint_turns.hpp).
//
// Engine thread.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::pose_animation::players {

// Every frame for each player: their field, on their actor (null while it has no body). A field
// that names an animation defined by name ("R1" <L|N> <name>) plays it once this client has it.
// A key's lift raises or lowers the hips (joint_turns::set_shift).
void show(std::uint32_t player, void* actor, const std::string& field);

// { op: 'poseAnimationDefine', name, field } from the client worker (the server's reserved client
// event kcds:poseAnimation, Animations.define longer than the field): kept by its name; "" field
// forgets it. Any thread.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// The player's physical prop settings the animation's keys set at the time it plays now
// (kcd2s/integration/pose_animation.hpp pose_prop_at), by code; empty while it
// plays none or sets none.
std::map<char, float> prop_now(std::uint32_t player);

// The whip's lash shape the player's animation gives at the time it plays now (its keys' lash,
// kcd2s/integration/pose_animation.hpp pose_lash_at): points from the handle's tip in the
// handle's frame (along, right, up); empty while it gives none (the lash moves by physics then).
std::vector<std::array<float, 3>> lash_now(std::uint32_t player);

// Whether that lash shape is in the player's body frame (forward, right, up: a lash track kept
// so) instead of the handle's.
bool lash_in_body(std::uint32_t player);

// The players not in the list are gone: their bodies are left as the game animates them.
void keep_only(const std::vector<std::uint32_t>& players);

}  // namespace kcds::runtime::pose_animation::players
