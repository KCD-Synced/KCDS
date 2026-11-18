#pragma once
#include <array>
#include <string>
#include <string_view>

// What a player wears on their head besides their hair and clothing (Player.crown,
// Player.hairCrown): the server keeps it in the NetworkPlayer field "headPiece", which every
// client puts on that player's body, the owner's own too, players who join later as well.
//
//   ""                    the default: no crown, and a hair that carries a crown shows it
//   "<crown>|<hair>"      <crown> one of kCrowns or "" for none; <hair> 1 when a hair that
//                         carries a crown (Sigismund's, m_hair_zikmund) shows it, 0 when not
//
// A crown is drawn from the game's own mesh it is part of, with that mesh's other parts not drawn
// (Client/runtime/appearance/head_pieces.hpp), over the player's own hair.
namespace kcd2s::integration {

// The crowns a player can wear: Sigismund's, the crown of his hair (m_hair_zikmund), and the
// game's two wreaths for a man's head that no item of the game puts on.
inline constexpr std::array<std::string_view, 3> kCrowns{"sigismund", "wreath", "twitchWreath"};
inline constexpr std::size_t kMaxHeadPieceField = 64;

struct HeadPiece {
  std::string crown;
  bool hair_crown = true;

  friend bool operator==(const HeadPiece& a, const HeadPiece& b) {
    return a.crown == b.crown && a.hair_crown == b.hair_crown;
  }
};

inline bool known_crown(std::string_view crown) {
  for (const std::string_view known : kCrowns) {
    if (known == crown) {
      return true;
    }
  }
  return false;
}

inline std::string head_piece_field(const HeadPiece& piece) {
  if (piece.crown.empty() && piece.hair_crown) {
    return std::string();
  }
  return piece.crown + (piece.hair_crown ? "|1" : "|0");
}

// False for a field no server writes (an unknown crown, another hair flag, no separator).
inline bool parse_head_piece_field(const std::string& field, HeadPiece& piece) {
  if (field.empty()) {
    piece = HeadPiece{};
    return true;
  }
  if (field.size() > kMaxHeadPieceField) {
    return false;
  }
  const std::size_t bar = field.find('|');
  if (bar == std::string::npos || bar + 2 != field.size()) {
    return false;
  }
  const std::string crown = field.substr(0, bar);
  const char hair = field[bar + 1];
  if ((!crown.empty() && !known_crown(crown)) || (hair != '0' && hair != '1')) {
    return false;
  }
  piece.crown = crown;
  piece.hair_crown = hair == '1';
  return true;
}

}  // namespace kcd2s::integration
