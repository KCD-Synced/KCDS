// The players' physical props in a server's world (Asset/Animation Studio): the
// rope or whip the server gave each player (Player.setPhysicalProp; the NetworkPlayer field
// physicalProp, kcd2s/integration/physical_prop.hpp), shown by every client on that player's
// body: the local player's and every replica's.
//
// Each client builds its own copy (a RopeProp, or a WhipProp whose handle entity is named
// kcds_physprop_<player>) and every frame ties its start to that body's hand (props/prop_entity
// joint_pose on the actor's entity, CActor + 56), with the field's hand, grip and physics and
// the player's animation's prop keys on top. The hand moves with the player's
// synchronised animation, so the prop goes where the player goes; the rope itself swings by
// each client's own physics (not sent).
//
// Engine thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::physical_props::players {

// Every frame, for each player in the world: its body's actor (null when it has none here) and
// its field's text ("" for none).
void show(std::uint32_t player, void* actor, const std::string& field);

// After the frame's show() calls: takes away the props of the players not shown.
void keep_only(const std::vector<std::uint32_t>& shown);

// The world went: every prop goes.
void reset();

}  // namespace kcds::runtime::physical_props::players
