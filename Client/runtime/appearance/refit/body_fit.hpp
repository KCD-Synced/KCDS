// A man's piece fitted to a woman's body with math: the piece changes, not her body or skeleton.
//
// The engine skins a piece with the piece's own bind pose, so on her skeleton each of its vertices
// follows her joints rigidly (carry(): her bind times the inverse of the piece's bind, per joint; a
// joint of the piece her skeleton lacks, as a strap or a pouch, follows its nearest ancestor she
// has). That fits her bone lengths but keeps the man's volume: a gambeson stood a few centimetres
// off her waist and her chest pushed through it.
//
// BodyField measures the difference of the two bodies. The male body, carried onto her skeleton the
// same way, is moved along each vertex's normal onto her body (the nearest of her vertices in a
// thin tube around the normal); the moves are smoothed over neighbours. fit() then moves each
// vertex of a piece by the mean move of the nearest carried male body vertices, fading out away
// from the body, and writes it back in the piece's own bind space, so the engine's skinning puts
// it on her body. Every piece takes the same field, so layers (a gambeson under a brigandine) keep
// their order.
#pragma once

#include <array>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include "skin_file.hpp"

namespace kcds::runtime::appearance::refit {

// For each joint of `piece_joints`: the transform that carries its bind pose onto `her_joints`.
std::vector<Matrix34> carry(const std::vector<Joint>& piece_joints,
                            const std::vector<Joint>& her_joints);

struct FieldSettings {
  // The tube around a male body normal in which her body is looked for, and how far along it.
  float tube_radius = 0.03f;
  float reach = 0.15f;
  // Smoothing: neighbours within this distance, this many rounds.
  float smooth_radius = 0.04f;
  int smooth_rounds = 3;
  // A piece vertex takes the mean of this many nearest body vertices, and no move at all from
  // this far off the body.
  std::size_t nearest = 8;
  float fade = 0.25f;
};

class BodyField {
 public:
  // The field from the male body (positions, normals and bone map of the game's male body skin)
  // to her body (positions in her bind space) on her skeleton. False when the male body has no
  // normals or no vertex of it found her body.
  bool build(const SkinFile& male_body, const std::vector<Joint>& her_joints,
             const std::vector<float>& her_body, const FieldSettings& settings = {});

  // The move at a point of her space (zero far from the body).
  std::array<float, 3> move_at(const std::array<float, 3>& point) const;

  std::size_t size() const { return points_.size(); }

 private:
  struct Cell {
    int x = 0;
    int y = 0;
    int z = 0;
    bool operator==(const Cell& other) const {
      return x == other.x && y == other.y && z == other.z;
    }
  };
  struct CellHash {
    std::size_t operator()(const Cell& cell) const;
  };

  Cell cell_of(const std::array<float, 3>& point) const;

  FieldSettings settings_;
  float cell_size_ = 0.05f;
  std::vector<std::array<float, 3>> points_;
  std::vector<std::array<float, 3>> moves_;
  std::unordered_map<Cell, std::vector<std::size_t>, CellHash> grid_;
};

// The piece's positions moved onto her body: `piece` read from its .skin, `carried` from carry()
// for its joints. Empty when the piece has no mesh.
std::vector<float> fit(const SkinFile& piece, const std::vector<Matrix34>& carried,
                       const BodyField& field);

}  // namespace kcds::runtime::appearance::refit
