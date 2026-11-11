// Pointing: while the local player holds the pointing key (B by its place on the
// keyboard), their right arm points with a finger where their camera looks and follows the camera;
// every other player sees the arm move. A server may turn it off (the interaction "pointing",
// INTERACTION_RULES' kRefusePointing).
//
// The pose is the game's own: the crowd's pointing clip (crowd_peasant_male_audience_point_03),
// held at kPointTime where the arm is out and the index finger points, played as a request of the
// client's own (player_animation::play_local: the upper body only, held still, so the legs walk
// and every other client holds the replica's clip at the same place through the request's speed
// mark). It starts only while no request of the server's runs.
//
// The aim is a pose modifier on the pose the body is drawn with (stage 2, pushed by the
// character's animation pass, upper_body::add_pass_listener): it turns the upper arm (the parent of
// RightForeArm) so the line from the shoulder to the tip of the index finger runs to the target,
// and moves everything that hangs from it along. The owner's target is the camera's line of sight,
// kTargetDistance ahead; it travels in front of the owner's animation field ("P" and the point,
// animation_props.hpp pointing_field), and every other client aims the replica's arm at it.
//
// Engine thread, except the modifier, which runs in the game's animation jobs.
#pragma once

#include <optional>
#include <string>

namespace kcds::runtime::animation::pointing {

using Logger = void (*)(const char* format, ...);

struct Natives {
  // The camera's matrix (12 floats: right, forward and up columns, the position in the 4th),
  // false without one.
  bool (*camera)(float matrix[12]) = nullptr;
  // Whether the pointing key is down and the game has the keyboard (no chat, page or menu).
  bool (*key_down)() = nullptr;
  // Whether the server lets the player point.
  bool (*allowed)() = nullptr;
  // Turns the game's action map of that name on or off (quick_access_key.hpp).
  void (*enable_action_map)(const char* name, bool enable) = nullptr;
  Logger log = nullptr;
};

// Listens to the characters' animation pass.
void install(const Natives& natives);

// Every frame with the local body: starts, aims and ends the local player's pointing.
// `controller`: the local player's action controller.
void keep_local(void* controller, void* local_entity);

// The local player's pointing target for the animation field, none while not pointing.
std::optional<std::string> local_mark();

// Every frame with a replica's body: its arm points at the target its owner's field names ("" for
// none); one not named again for a moment is let go.
void keep_replica(void* entity, const std::string& mark);

// The world went away.
void reset();

}  // namespace kcds::runtime::animation::pointing
