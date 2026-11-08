#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

// A lead rope a player holds (Player.setLeadRope): from a joint of the player's body to a joint
// of another body (a player, an NPC or a horse), drawn every frame by every client. The
// NetworkPlayer field `leadRope` carries it to every client, a player who joins later too, as
// text: "<target network id>|<the holder's joint>|<the target's joint>|<length in metres>"
// ("12|RightHand|Spine|2.50"), "" for none.
namespace kcd2s::integration {

struct LeadRope {
  std::uint32_t target = 0;
  std::string from = "RightHand";
  std::string to = "Spine";
  float length = 2.5f;
};

inline constexpr float kLeadRopeShortest = 0.5f;
inline constexpr float kLeadRopeLongest = 20.0f;
inline constexpr std::size_t kLeadRopeJointMax = 48;

// A joint's name: printable, no '|', at most kLeadRopeJointMax characters.
inline bool lead_rope_joint_fits(const std::string& joint) {
  if (joint.empty() || joint.size() > kLeadRopeJointMax) {
    return false;
  }
  for (const char c : joint) {
    if (c < 0x20 || c > 0x7e || c == '|') {
      return false;
    }
  }
  return true;
}

inline bool lead_rope_fits(const LeadRope& rope) {
  return rope.target != 0 && lead_rope_joint_fits(rope.from) && lead_rope_joint_fits(rope.to) &&
         rope.length >= kLeadRopeShortest && rope.length <= kLeadRopeLongest;
}

inline std::string lead_rope_field(const LeadRope& rope) {
  char length[32];
  std::snprintf(length, sizeof(length), "%.2f", double(rope.length));
  return std::to_string(rope.target) + "|" + rope.from + "|" + rope.to + "|" + length;
}

// The rope of a field's text; nullopt for "" and for anything that does not fit.
inline std::optional<LeadRope> lead_rope_of(const std::string& field) {
  const std::size_t first = field.find('|');
  const std::size_t second = first == std::string::npos ? first : field.find('|', first + 1);
  const std::size_t third = second == std::string::npos ? second : field.find('|', second + 1);
  if (third == std::string::npos || field.find('|', third + 1) != std::string::npos) {
    return std::nullopt;
  }
  LeadRope rope;
  const std::string target = field.substr(0, first);
  char* end = nullptr;
  const unsigned long long id = std::strtoull(target.c_str(), &end, 10);
  if (target.empty() || *end != '\0' || id == 0 || id > 0xffffffffull) {
    return std::nullopt;
  }
  rope.target = std::uint32_t(id);
  rope.from = field.substr(first + 1, second - first - 1);
  rope.to = field.substr(second + 1, third - second - 1);
  const std::string length = field.substr(third + 1);
  rope.length = std::strtof(length.c_str(), &end);
  if (length.empty() || *end != '\0' || !lead_rope_fits(rope)) {
    return std::nullopt;
  }
  return rope;
}

}  // namespace kcd2s::integration
