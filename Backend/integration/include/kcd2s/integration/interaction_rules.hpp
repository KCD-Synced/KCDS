#pragma once
#include <cstdint>
#include <vector>

// The game's own interactions a server allows a player (KCDS addition, the server's
// Interactions.set and Player.setInteraction): off by default, so a KCDS server starts without
// the game's single-player prompts and keys on its people, and a resource turns on what it wants.
//
//   INTERACTION_RULES : server -> a player's client, on the control channel (0):
//                       [tag][u16 allowed bits, low byte first]. Sent when the player's world gets
//                       ready and whenever what they are allowed changes. A client that leaves a
//                       world goes back to nothing allowed.
namespace kcd2s::integration {

inline constexpr std::uint8_t kInteractionRulesTag = 143;

// The interactions, one bit each.
inline constexpr std::uint16_t kAllowHorseWhistle = 1;  // the companion key's whistle (call)
inline constexpr std::uint16_t kAllowNpcTalk = 2;       // "Talk" on an NPC
inline constexpr std::uint16_t kAllowNpcChat = 4;       // the focus chat on an NPC (provoking)
inline constexpr std::uint16_t kAllowPlayerTalk = 8;    // "Talk" and the focus chat on a player
// The NPCs' own talk (barks, chatter, their lines in a fight) is on unless this bit is set: the
// game's dialog system, which its wh_dlg_Enable switches off ("If disabled all NPC's have nothing
// to say"). Inverted so that a client never told keeps the game's talk.
inline constexpr std::uint16_t kRefuseNpcChatter = 16;
// The pointing key works unless this bit is set; inverted as kRefuseNpcChatter.
inline constexpr std::uint16_t kRefusePointing = 32;
// The player may be in private and prohibited areas: the game's warning stays hidden
// and the game's AI is told not to react to them being there.
inline constexpr std::uint16_t kAllowTrespass = 64;
// The game's food stations, on unless these bits are set: cooking at a fire, smoking
// and drying at a smokehouse or a drying rack.
inline constexpr std::uint16_t kRefuseCooking = 128;
inline constexpr std::uint16_t kRefuseSmoking = 256;
inline constexpr std::uint16_t kAllowedBits =
    kAllowHorseWhistle | kAllowNpcTalk | kAllowNpcChat | kAllowPlayerTalk | kRefuseNpcChatter |
    kRefusePointing | kAllowTrespass | kRefuseCooking | kRefuseSmoking;

inline std::vector<std::uint8_t> encode_interaction_rules(std::uint16_t allowed) {
  const std::uint16_t bits = allowed & kAllowedBits;
  return {kInteractionRulesTag, std::uint8_t(bits & 0xff), std::uint8_t(bits >> 8)};
}

inline bool decode_interaction_rules(const std::vector<std::uint8_t>& p, std::uint16_t& allowed) {
  if (p.size() != 3 || p[0] != kInteractionRulesTag) {
    return false;
  }
  const std::uint16_t bits = std::uint16_t(p[1] | (p[2] << 8));
  if ((bits & ~kAllowedBits) != 0) {
    return false;
  }
  allowed = bits;
  return true;
}

}  // namespace kcd2s::integration
