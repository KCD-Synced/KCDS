#include "custom_navmesh.hpp"

#include <algorithm>
#include <cstring>

namespace kcds::runtime::maps::custom {
namespace {

constexpr std::int32_t kMagic = 0x444E4156;
constexpr std::int32_t kVersion = 14;
constexpr int kTile = 16;
constexpr int kCells = 4;
constexpr float kStep = float(kTile) / float(kCells);
constexpr std::uint8_t kWalk = 1;
constexpr std::uint16_t kBorder = 0x8000;
constexpr std::uint16_t kSidePlusX = 0;
constexpr std::uint16_t kSidePlusY = 2;
constexpr std::uint16_t kSideMinusX = 4;
constexpr std::uint16_t kSideMinusY = 6;
constexpr float kWalkableHeight = 1.76f;
constexpr float kWalkableRadius = 0.32f;
constexpr float kWalkableClimb = 0.32f;
constexpr float kFourth = 0.8f;
constexpr float kQuant = 12.5f;
constexpr float kBoxMargin = 0.5f;

void put_int(std::vector<std::uint8_t>& out, std::int32_t value) {
  std::uint8_t bytes[4];
  std::memcpy(bytes, &value, 4);
  out.insert(out.end(), bytes, bytes + 4);
}

void put_float(std::vector<std::uint8_t>& out, float value) {
  std::uint8_t bytes[4];
  std::memcpy(bytes, &value, 4);
  out.insert(out.end(), bytes, bytes + 4);
}

void put_short(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(std::uint8_t(value & 0xFF));
  out.push_back(std::uint8_t(value >> 8));
}

// The navigation settings the game's own navmeshes start with (Scripts/AI/Navigation.xml: tile
// size 200 voxels of 0.08 m, radius 4, height 22, climb 4, slope 45), as klaster's .nav has them.
void put_settings(std::vector<std::uint8_t>& out) {
  put_int(out, 100);
  put_float(out, 2.0f);
  put_int(out, 200);
  put_int(out, 4);
  put_int(out, 400);
  put_int(out, 3);
  put_float(out, 1.0f);
  put_float(out, 0.08f);
  put_float(out, 0.08f);
  put_float(out, 0.08f);
  put_int(out, 4);
  put_int(out, 22);
  put_int(out, 4);
  put_float(out, 45.0f);
}

bool blocked_at(const std::vector<Blocked>& blocked, float x, float y) {
  return std::any_of(blocked.begin(), blocked.end(), [x, y](const Blocked& area) {
    return x >= area.min_x && x <= area.max_x && y >= area.min_y && y <= area.max_y;
  });
}

// One tile; false when none of its squares is walkable.
bool make_tile(int tile_x, int tile_y, const HeightAt& height_at,
               const std::vector<Blocked>& blocked, std::vector<std::uint8_t>& out,
               NavmeshFiles& files) {
  const float left = float(tile_x * kTile);
  const float bottom = float(tile_y * kTile);
  // The corners and their heights.
  std::vector<float> heights;
  for (int j = 0; j <= kCells; ++j) {
    for (int i = 0; i <= kCells; ++i) {
      float height = 0;
      if (!height_at(left + float(i) * kStep, bottom + float(j) * kStep, height)) {
        return false;
      }
      heights.push_back(height);
    }
  }
  // Which squares are walkable, and their polygon numbers.
  int kept[kCells][kCells];
  int poly_count = 0;
  for (int j = 0; j < kCells; ++j) {
    for (int i = 0; i < kCells; ++i) {
      const float middle_x = left + (float(i) + 0.5f) * kStep;
      const float middle_y = bottom + (float(j) + 0.5f) * kStep;
      if (blocked_at(blocked, middle_x, middle_y)) {
        kept[j][i] = -1;
        files.left_out += 1;
      } else {
        kept[j][i] = poly_count;
        poly_count += 1;
      }
    }
  }
  if (poly_count == 0) {
    return false;
  }
  files.walkable += poly_count;
  // A neighbour: its polygon number + 1, 0 for a wall, or the tile's side on its edge.
  auto neighbour = [&kept](int i, int j, std::uint16_t side) -> std::uint16_t {
    if (i < 0 || j < 0 || i >= kCells || j >= kCells) {
      return std::uint16_t(kBorder | side);
    }
    return kept[j][i] < 0 ? 0 : std::uint16_t(kept[j][i] + 1);
  };
  const auto [lowest, highest] = std::minmax_element(heights.begin(), heights.end());
  const int vertex_count = (kCells + 1) * (kCells + 1);
  // The header: 17 ints, then the walk settings, the box and the quantize factor.
  put_int(out, kMagic);
  put_int(out, kVersion);
  put_int(out, tile_x);
  put_int(out, tile_y);
  put_int(out, 0);
  put_int(out, 0);
  put_int(out, poly_count);
  put_int(out, vertex_count);
  put_int(out, 0);
  put_int(out, poly_count * 8);
  put_int(out, poly_count);
  for (int unused = 0; unused < 5; ++unused) {
    put_int(out, 0);
  }
  put_int(out, poly_count);
  put_float(out, kWalkableHeight);
  put_float(out, kWalkableRadius);
  put_float(out, kWalkableClimb);
  put_float(out, kFourth);
  put_float(out, left);
  put_float(out, bottom);
  put_float(out, *lowest - kBoxMargin);
  put_float(out, left + float(kTile));
  put_float(out, bottom + float(kTile));
  put_float(out, *highest + kBoxMargin);
  put_float(out, kQuant);
  // The vertices.
  for (int j = 0; j <= kCells; ++j) {
    for (int i = 0; i <= kCells; ++i) {
      put_float(out, left + float(i) * kStep);
      put_float(out, bottom + float(j) * kStep);
      put_float(out, heights[std::size_t(j * (kCells + 1) + i)]);
    }
  }
  // The squares, counter-clockwise seen from above; edge k from corner k to k + 1: bottom (-y),
  // right (+x), top (+y), left (-x).
  for (int j = 0; j < kCells; ++j) {
    for (int i = 0; i < kCells; ++i) {
      if (kept[j][i] < 0) {
        continue;
      }
      const int corner = j * (kCells + 1) + i;
      const std::uint16_t ring[4] = {std::uint16_t(corner), std::uint16_t(corner + 1),
                                     std::uint16_t(corner + kCells + 2),
                                     std::uint16_t(corner + kCells + 1)};
      const std::uint16_t sides[4] = {
          neighbour(i, j - 1, kSideMinusY), neighbour(i + 1, j, kSidePlusX),
          neighbour(i, j + 1, kSidePlusY), neighbour(i - 1, j, kSideMinusX)};
      for (const std::uint16_t vertex : ring) {
        put_short(out, vertex);
      }
      put_short(out, 0);
      put_short(out, 0);
      for (const std::uint16_t side : sides) {
        put_short(out, side);
      }
      put_short(out, 0);
      put_short(out, 0);
      out.push_back(kWalk);
      out.push_back(4);
      out.push_back(0);
      out.push_back(1);
    }
  }
  return true;
}

}  // namespace

NavmeshFiles make_navmesh(int size, const HeightAt& height_at,
                          const std::vector<Blocked>& blocked) {
  NavmeshFiles files;
  const int tiles = size / kTile;
  std::vector<std::uint8_t> body;
  std::vector<std::pair<int, int>> places;
  std::vector<std::pair<std::size_t, std::size_t>> spans;
  for (int tile_y = 0; tile_y < tiles; ++tile_y) {
    for (int tile_x = 0; tile_x < tiles; ++tile_x) {
      const std::size_t start = body.size();
      if (!make_tile(tile_x, tile_y, height_at, blocked, body, files)) {
        body.resize(start);
        continue;
      }
      places.emplace_back(tile_x, tile_y);
      spans.emplace_back(start, body.size() - start);
    }
  }
  files.tile_count = int(places.size());
  put_settings(files.tiles);
  put_int(files.tiles, files.tile_count);
  const std::size_t head = files.tiles.size();
  files.tiles.insert(files.tiles.end(), body.begin(), body.end());
  for (std::size_t tile = 0; tile < places.size(); ++tile) {
    put_int(files.index, places[tile].first);
    put_int(files.index, places[tile].second);
    put_int(files.index, std::int32_t(head + spans[tile].first));
    put_int(files.index, 1);
    put_int(files.index, std::int32_t(spans[tile].second));
  }
  return files;
}

}  // namespace kcds::runtime::maps::custom
