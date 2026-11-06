#pragma once
// A player's dodge on every screen, WHGame 1.5.5.
//
// In a locked fight the game dodges on the jump key with a direction held: the combat input's jump
// handler (registered for the action registry's jump at + 40) calls the dodge
// request with the fighter's combat controller (movement + 1104). The request needs a
// lock target (core + 3320) and an input direction: the controller's first vector at + 104 (the
// sum of the movement keys held: right (1, 0), forward (0, 1), left (-1, 0), back (0, -1))
// when it has a length, else its second at + 112. It normalizes it, finds the dodge row
// for it and the target and creates the dodge
// (C_CombatActorActionDodge, action 8), whose start sets combat state 512 (Dodge).
//
// The owner's client relays the direction of every dodge its game made; the replica's game makes
// the same native dodge through its own request in that direction. These helpers are the parts of
// that without native objects.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "kcd2s/integration/combat.hpp"

namespace kcds::runtime::combat {

struct DodgeVector {
  float x = 0.0f;  // right
  float y = 0.0f;  // forward
};

constexpr float kDegreesPerRadian = 57.29578f;
using kcd2s::integration::kMaxDodgeDegrees;

// The direction the game's dodge request dodges in, from the controller's two input vectors, in
// degrees (0 right, 90 forward, 180 left, -90 back); nullopt when neither has a length (the
// request makes no dodge then).
inline std::optional<float> dodge_degrees(DodgeVector keys, DodgeVector second) {
  for (const DodgeVector& vector : {keys, second}) {
    if (!std::isfinite(vector.x) || !std::isfinite(vector.y)) {
      continue;
    }
    if (vector.x * vector.x + vector.y * vector.y > 0.0f) {
      // Within a half turn either way also after the float rounding of a straight left.
      const float degrees = std::atan2(vector.y, vector.x) * kDegreesPerRadian;
      return std::clamp(degrees, -kMaxDodgeDegrees, kMaxDodgeDegrees);
    }
  }
  return std::nullopt;
}

// The unit input vector of a relayed dodge direction.
inline DodgeVector dodge_vector(float degrees) {
  const float radians = degrees / kDegreesPerRadian;
  return DodgeVector{std::cos(radians), std::sin(radians)};
}

// A blow another player's game relayed onto the local player, answered by the dodger's own screen
// (the dodger's screen counts). The attacker's game hit the replica, whose position runs
// about a hop plus the 100 ms replica timeline behind its owner; the contact reaches the dodger
// about 100 ms before their own screen shows the blade arriving. So:
//   - a dodge the dodger's game made in its slot (a dodge window open) from kDodgeBeforeMs before
//     the contact arrived until now takes the blow out (Dodged);
//   - while a dodge window is open on the local fighter (the swing still coming on their screen)
//     the contact waits, at most kDodgeWaitMs after it arrived (Wait);
//   - otherwise it is taken at once, as before (Take).
// A dodge out of its slot leaves the blow to the attacker's contact, as in the game.
enum class DodgeAnswer { Take, Wait, Dodged };
constexpr std::uint64_t kDodgeBeforeMs = 400;
constexpr std::uint64_t kDodgeWaitMs = 300;

// `now` and `arrived` in ms; `slot_dodge_at` the last slot dodge's time (0 for none) and
// `windows` the dodge windows open on the local fighter now.
inline DodgeAnswer answer_contact(std::uint64_t now, std::uint64_t arrived,
                                  std::uint64_t slot_dodge_at, int windows) {
  if (slot_dodge_at != 0 && slot_dodge_at <= now && slot_dodge_at + kDodgeBeforeMs >= arrived) {
    return DodgeAnswer::Dodged;
  }
  if (windows > 0 && now < arrived + kDodgeWaitMs) {
    return DodgeAnswer::Wait;
  }
  return DodgeAnswer::Take;
}

}  // namespace kcds::runtime::combat
