#pragma once
#include <cstdint>

// How a player's body stands: whether it is off the ground, whether it is crouched,
// and the physics profile it is in, as the owner's own game reports them. The owner publishes
// them in the NetworkPlayer field "posture" (posture_field, an I64):
//
//   bit 0       in the air: its living physics has been flying for longer than a stair step
//   bit 1       crouched: the owner's own animation stance is its crouch
//   bit 2       reported (0 for a player whose client has not reported yet)
//   bit 3       guarding: its soul holds the guard (state flag IsBlocking)
//   bit 4       weapon or fists raised (state flag HasRaisedWeapon)
//   bit 5       on a ladder (its game's IsOnLadder)
//   bit 6       fainted: it lies in the game's own exhaustion faint, which its game ends by
//               itself, not knocked out
//   bits 8-15   the physics profile of its game object (Client/runtime/body_state), 255 for none
//   bits 16-23  the combat-star direction its raised weapon or fists aim at, as a row of the
//               game's zone table, plus one; 0 when it aims at none
//   bits 32-47  on a ladder, while its game holds the climb clip on its base layer by hand (the
//               ladder action sets that clip's normalized time as the player climbs and keeps it
//               while it stops): 1 plus that time in 65534ths; 0 while its game plays
//               its base layer on its own clock (getting on or off the ladder) and off a ladder
//   bits 48-55  the stance tag the owner's own animation chose for its body, plus one (the
//               Mannequin tag id of its Stance group, as Player.bodyStance reads it; a seated body
//               has the sitting stance); 0 for none
//
// Anything else is not a posture; a player without one reads the defaults.
namespace kcd2s::integration {

inline constexpr std::uint8_t kNoPhysicsProfile = 255;
// A body whose stance group holds no tag.
inline constexpr std::uint8_t kNoStance = 255;

struct PlayerPosture {
  bool in_air = false;
  bool crouched = false;
  bool guard = false;
  bool raised = false;
  bool on_ladder = false;
  bool fainted = false;
  std::uint8_t physics_profile = kNoPhysicsProfile;
  std::int16_t combat_zone = -1;      // -1: none; 0 to 254
  std::uint16_t ladder_progress = 0;  // a held climb clip's time (ladder_progress_of), or 0
  std::uint8_t stance = kNoStance;    // the Stance tag id, 0 to 254, or kNoStance

  friend bool operator==(const PlayerPosture& a, const PlayerPosture& b) {
    return a.in_air == b.in_air && a.crouched == b.crouched && a.guard == b.guard &&
           a.raised == b.raised && a.on_ladder == b.on_ladder && a.fainted == b.fainted &&
           a.physics_profile == b.physics_profile && a.combat_zone == b.combat_zone &&
           a.ladder_progress == b.ladder_progress && a.stance == b.stance;
  }
};

inline constexpr std::int64_t kPostureInAir = 1;
inline constexpr std::int64_t kPostureCrouched = 2;
inline constexpr std::int64_t kPostureReported = 4;
inline constexpr std::int64_t kPostureGuard = 8;
inline constexpr std::int64_t kPostureRaised = 16;
inline constexpr std::int64_t kPostureOnLadder = 32;
inline constexpr std::int64_t kPostureFainted = 64;
inline constexpr std::int64_t kPostureCombatZone = 0xFF0000;
inline constexpr std::int64_t kPostureLadderProgress = 0xFFFF00000000;
inline constexpr std::int64_t kPostureStance = 0xFF000000000000;

// A held climb clip's normalized time (0 to 1) as ladder_progress, never 0: a climber resting at
// the clip's start is still held there.
inline std::uint16_t ladder_progress_of(float normalized_time) {
  if (!(normalized_time > 0.0f)) {
    return 1;
  }
  if (normalized_time >= 1.0f) {
    return 65535;
  }
  return std::uint16_t(1.0f + normalized_time * 65534.0f + 0.5f);
}

// Whether a ladder_progress is a held climb clip's time.
inline bool ladder_clip_held(std::uint16_t progress) {
  return progress != 0;
}

// The normalized time of a held climb clip's ladder_progress, 0 for none.
inline float ladder_time_of(std::uint16_t progress) {
  if (!ladder_clip_held(progress)) {
    return 0.0f;
  }
  return float(progress - 1) / 65534.0f;
}

inline std::int64_t posture_field(const PlayerPosture& posture) {
  std::int64_t field = kPostureReported;
  if (posture.in_air) {
    field |= kPostureInAir;
  }
  if (posture.crouched) {
    field |= kPostureCrouched;
  }
  if (posture.guard) {
    field |= kPostureGuard;
  }
  if (posture.raised) {
    field |= kPostureRaised;
  }
  if (posture.on_ladder) {
    field |= kPostureOnLadder;
    field |= std::int64_t(posture.ladder_progress) << 32;
  }
  if (posture.fainted) {
    field |= kPostureFainted;
  }
  field |= std::int64_t(posture.physics_profile) << 8;
  if (posture.combat_zone >= 0 && posture.combat_zone < 255) {
    field |= std::int64_t(posture.combat_zone + 1) << 16;
  }
  if (posture.stance != kNoStance) {
    field |= std::int64_t(posture.stance + 1) << 48;
  }
  return field;
}

// The posture of a field; false (and posture left alone) for one that was never reported or is
// not one.
inline bool parse_posture_field(std::int64_t field, PlayerPosture& posture) {
  constexpr std::int64_t kKnownBits = kPostureInAir | kPostureCrouched | kPostureReported |
                                      kPostureGuard | kPostureRaised | kPostureOnLadder |
                                      kPostureFainted | 0xFF00 | kPostureCombatZone |
                                      kPostureLadderProgress | kPostureStance;
  if ((field & ~kKnownBits) != 0 || (field & kPostureReported) == 0) {
    return false;
  }
  if ((field & kPostureOnLadder) == 0 && (field & kPostureLadderProgress) != 0) {
    return false;
  }
  posture.in_air = (field & kPostureInAir) != 0;
  posture.crouched = (field & kPostureCrouched) != 0;
  posture.guard = (field & kPostureGuard) != 0;
  posture.raised = (field & kPostureRaised) != 0;
  posture.on_ladder = (field & kPostureOnLadder) != 0;
  posture.fainted = (field & kPostureFainted) != 0;
  posture.physics_profile = std::uint8_t(field >> 8);
  posture.combat_zone = std::int16_t(((field & kPostureCombatZone) >> 16) - 1);
  posture.ladder_progress = std::uint16_t((field & kPostureLadderProgress) >> 32);
  const std::int64_t stance = (field & kPostureStance) >> 48;
  posture.stance = stance == 0 ? kNoStance : std::uint8_t(stance - 1);
  return true;
}

}  // namespace kcd2s::integration
