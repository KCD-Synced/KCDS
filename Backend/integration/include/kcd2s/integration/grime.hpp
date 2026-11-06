#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Dirt, blood and wear.
//
//   the owner field "grime" : "KG1", the body's dirt and the blood of its six zones (7 bytes,
//                             each 0 .. 255 of 1), then per worn piece its 16-byte class id, the
//                             same 7 bytes and its wear (its health, 0 .. 255 of 1);
//                             Client/runtime/appearance/grime.hpp captures and applies it.
//   GRIME_ORDER (tag 47)    : server -> the player's own client, on the control channel (0): clean
//                             the body and/or the clothes, or soil them with dirt and blood.
namespace kcd2s::integration {

inline constexpr std::uint8_t kGrimeOrderTag = 47;
inline constexpr std::size_t kGrimeZones = 6;
inline constexpr std::size_t kMaxGrimePieces = 64;

struct GrimeState {
  std::uint8_t dirt = 0;
  std::array<std::uint8_t, kGrimeZones> blood{};
};

struct GrimePiece {
  std::array<std::uint8_t, 16> item{};
  GrimeState state;
  std::uint8_t wear = 255;
};

struct Grime {
  GrimeState body;
  std::vector<GrimePiece> pieces;
};

// The grime field; false for anything else (an empty field: not reported yet).
inline bool parse_grime_field(const std::string& field, Grime& grime) {
  constexpr std::size_t kHeader = 3 + 1 + kGrimeZones;
  constexpr std::size_t kPiece = 16 + 1 + kGrimeZones + 1;
  if (field.size() < kHeader || field.compare(0, 3, "KG1") != 0 ||
      (field.size() - kHeader) % kPiece != 0 ||
      (field.size() - kHeader) / kPiece > kMaxGrimePieces) {
    return false;
  }
  const auto byte = [&](std::size_t at) { return static_cast<std::uint8_t>(field[at]); };
  Grime parsed;
  parsed.body.dirt = byte(3);
  for (std::size_t zone = 0; zone < kGrimeZones; ++zone) {
    parsed.body.blood[zone] = byte(4 + zone);
  }
  for (std::size_t at = kHeader; at < field.size(); at += kPiece) {
    GrimePiece piece;
    for (std::size_t i = 0; i < piece.item.size(); ++i) {
      piece.item[i] = byte(at + i);
    }
    piece.state.dirt = byte(at + 16);
    for (std::size_t zone = 0; zone < kGrimeZones; ++zone) {
      piece.state.blood[zone] = byte(at + 17 + zone);
    }
    piece.wear = byte(at + 17 + kGrimeZones);
    parsed.pieces.push_back(piece);
  }
  grime = std::move(parsed);
  return true;
}

struct GrimeOrder {
  enum class Kind : std::uint8_t {
    Clean = 1,  // dirt and blood to 0
    Soil = 2,   // dirt and blood added (each zone), up to 255
  };
  Kind kind = Kind::Clean;
  bool body = true;
  bool clothes = true;
  std::uint8_t dirt = 0;   // Soil: the dirt added, 0 .. 255 of 1
  std::uint8_t blood = 0;  // Soil: the blood added to every zone, 0 .. 255 of 1

  friend bool operator==(const GrimeOrder& a, const GrimeOrder& b) {
    return a.kind == b.kind && a.body == b.body && a.clothes == b.clothes && a.dirt == b.dirt &&
           a.blood == b.blood;
  }
};

// GRIME_ORDER: [tag][u8 kind][u8 parts: 1 body, 2 clothes][u8 dirt][u8 blood]; empty for an
// unknown kind or no part.
inline std::vector<std::uint8_t> encode_grime_order(const GrimeOrder& order) {
  const auto kind = std::uint8_t(order.kind);
  if ((kind != 1 && kind != 2) || (!order.body && !order.clothes)) {
    return {};
  }
  const std::uint8_t parts = std::uint8_t((order.body ? 1 : 0) | (order.clothes ? 2 : 0));
  return {kGrimeOrderTag, kind, parts, order.dirt, order.blood};
}

inline bool decode_grime_order(const std::vector<std::uint8_t>& p, GrimeOrder& order) {
  if (p.size() != 5 || p[0] != kGrimeOrderTag || (p[1] != 1 && p[1] != 2) || p[2] == 0 ||
      p[2] > 3) {
    return false;
  }
  order.kind = GrimeOrder::Kind(p[1]);
  order.body = (p[2] & 1) != 0;
  order.clothes = (p[2] & 2) != 0;
  order.dirt = p[3];
  order.blood = p[4];
  return true;
}

}  // namespace kcd2s::integration
