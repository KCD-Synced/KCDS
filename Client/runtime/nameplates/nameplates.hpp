// The nameplates above players: each player's name, or the text the
// server gave their tag, in the tag's colour, a title above it, a health bar under it and a
// resource's label above them, as plates the in-frame overlay paints in the owner's design
// (nameplate_paint.hpp).
//
// What a tag shows comes with the player (kcd2s/integration/nameplates.hpp, the NetworkPlayer field
// "nameplate"); this player's own choices come from the client resources (Client/resources/
// nameplates.cjs: nameplateView, nameplateLabel, nameplateLabelsClear). Past 30 m a plate is the
// name alone, past 60 m nothing, and none is drawn behind terrain, buildings or other solid things
// (physics/line_of_sight.hpp). The plates go behind the game's HUD (nameplate_paint.hpp).
//
// Engine thread, but for handle_runtime_message.
#pragma once

#include <cstdint>
#include <string>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::nameplates {

// The top of the head from the skeleton's joints (metres; a test probe on the male body,
// feet 0, Head 1.537, HeadEnd 1.887, the hair's top about 1.76 from the screen):
// HeadEnd is above the hair, Head at the skull's base.
inline constexpr float kHeadEndAboveTop = 0.13f;
inline constexpr float kHeadJointToTop = 0.22f;

struct Seen {
  float feet[3] = {0.0f, 0.0f, 0.0f};
  float head[3] = {0.0f, 0.0f, 0.0f};  // the top of the head (joint HeadEnd), when has_head
  bool has_head = false;
  std::string nickname;
  std::string field;      // the NetworkPlayer field "nameplate"
  float health = -1.0f;   // 0 to 100, below 0 while unknown
  bool own = false;       // this player's own tag
  bool speaking = false;  // the player talks (Player.setSpeaking)
};

// This frame: a player and what their tag shows.
void keep(std::uint32_t id, const Seen& seen);

// Draws the tags kept since the last frame; `camera` is the view matrix (3 x 4, rows), null when
// there is none, and `fov` its vertical field of view in radians.
void frame(double now, const float* camera, float fov);

// A resource runtime message; false when it is not a nameplate's. Any thread: it is queued for the
// next frame.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// The session ends: the players and the labels go, the view switches stay.
void reset();

}  // namespace kcds::runtime::nameplates
