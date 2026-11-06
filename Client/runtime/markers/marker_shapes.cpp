#include "marker_shapes.hpp"

#include <cmath>

namespace kcds::runtime::markers::shapes {
namespace {

constexpr float kPi = 3.14159265f;
constexpr int kSegments = 24;
constexpr int kRings = 12;
// A plane floats this far over the ground so the ground does not hide it.
constexpr float kPlaneLift = 0.03f;
// A chevron's arms are this share of its width thick and deep.
constexpr float kChevronThickness = 0.25f;

std::uint32_t point(Mesh& mesh, float x, float y, float z) {
  mesh.points.push_back(x);
  mesh.points.push_back(y);
  mesh.points.push_back(z);
  return std::uint32_t(mesh.points.size() / 3 - 1);
}

void triangle(Mesh& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
  mesh.triangles.push_back(a);
  mesh.triangles.push_back(b);
  mesh.triangles.push_back(c);
}

void line(Mesh& mesh, std::uint32_t a, std::uint32_t b) {
  mesh.lines.push_back(a);
  mesh.lines.push_back(b);
}

// A level ring of points about the vertical at height `z`; its first index.
std::uint32_t ring(Mesh& mesh, float radius, float z, bool outline) {
  const std::uint32_t first = std::uint32_t(mesh.points.size() / 3);
  for (int i = 0; i < kSegments; ++i) {
    const float angle = 2.0f * kPi * float(i) / float(kSegments);
    point(mesh, radius * std::cos(angle), radius * std::sin(angle), z);
  }
  if (outline) {
    for (int i = 0; i < kSegments; ++i) {
      line(mesh, first + i, first + (i + 1) % kSegments);
    }
  }
  return first;
}

void band(Mesh& mesh, std::uint32_t lower, std::uint32_t upper) {
  for (int i = 0; i < kSegments; ++i) {
    const std::uint32_t next = (i + 1) % kSegments;
    triangle(mesh, lower + i, lower + next, upper + next);
    triangle(mesh, lower + i, upper + next, upper + i);
  }
}

void cap(Mesh& mesh, std::uint32_t first, float z) {
  const std::uint32_t middle = point(mesh, 0.0f, 0.0f, z);
  for (int i = 0; i < kSegments; ++i) {
    triangle(mesh, middle, first + i, first + (i + 1) % kSegments);
  }
}

void cylinder(Mesh& mesh, float radius, float height) {
  const std::uint32_t lower = ring(mesh, radius, 0.0f, true);
  const std::uint32_t upper = ring(mesh, radius, height, true);
  band(mesh, lower, upper);
  cap(mesh, upper, height);
  for (int i = 0; i < 4; ++i) {
    const std::uint32_t at = std::uint32_t(i * kSegments / 4);
    line(mesh, lower + at, upper + at);
  }
}

void sphere(Mesh& mesh, float radius) {
  std::uint32_t previous = 0;
  const std::uint32_t south = point(mesh, 0.0f, 0.0f, -radius);
  for (int step = 1; step < kRings; ++step) {
    const float angle = kPi * float(step) / float(kRings) - kPi / 2.0f;
    const std::uint32_t current =
        ring(mesh, radius * std::cos(angle), radius * std::sin(angle), step == kRings / 2);
    if (step == 1) {
      for (int i = 0; i < kSegments; ++i) {
        triangle(mesh, south, current + (i + 1) % kSegments, current + i);
      }
    } else {
      band(mesh, previous, current);
    }
    previous = current;
  }
  const std::uint32_t north = point(mesh, 0.0f, 0.0f, radius);
  for (int i = 0; i < kSegments; ++i) {
    triangle(mesh, north, previous + i, previous + (i + 1) % kSegments);
  }
}

// A box from `low` to `high` with its faces and twelve edges.
void box(Mesh& mesh, const float low[3], const float high[3]) {
  std::uint32_t corners[8];
  for (int i = 0; i < 8; ++i) {
    corners[i] = point(mesh, (i & 1) ? high[0] : low[0], (i & 2) ? high[1] : low[1],
                       (i & 4) ? high[2] : low[2]);
  }
  for (int i = 0; i < 8; ++i) {
    for (int bit = 1; bit < 8; bit <<= 1) {
      if ((i & bit) == 0) {
        line(mesh, corners[i], corners[i | bit]);
      }
    }
  }
  static const int kFaces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                   {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& face : kFaces) {
    triangle(mesh, corners[face[0]], corners[face[1]], corners[face[2]]);
    triangle(mesh, corners[face[0]], corners[face[2]], corners[face[3]]);
  }
}

void plane(Mesh& mesh, float size) {
  const float half = size / 2.0f;
  const std::uint32_t a = point(mesh, -half, -half, kPlaneLift);
  const std::uint32_t b = point(mesh, half, -half, kPlaneLift);
  const std::uint32_t c = point(mesh, half, half, kPlaneLift);
  const std::uint32_t d = point(mesh, -half, half, kPlaneLift);
  triangle(mesh, a, b, c);
  triangle(mesh, a, c, d);
  line(mesh, a, b);
  line(mesh, b, c);
  line(mesh, c, d);
  line(mesh, d, a);
}

// A V in the X-Z plane, its point down at `bottom`, `size` wide, as a solid of some depth.
void chevron(Mesh& mesh, float size, float bottom) {
  const float half = size / 2.0f;
  const float thick = size * kChevronThickness;
  const float depth = thick / 2.0f;
  // The outline of the V, inner and outer edges, going round.
  const float outline[6][2] = {
      {0.0f, bottom},         {half, bottom + half},          {half - thick, bottom + half},
      {0.0f, bottom + thick}, {-half + thick, bottom + half}, {-half, bottom + half},
  };
  std::uint32_t front[6];
  std::uint32_t back[6];
  for (int i = 0; i < 6; ++i) {
    front[i] = point(mesh, outline[i][0], -depth, outline[i][1]);
    back[i] = point(mesh, outline[i][0], depth, outline[i][1]);
  }
  // Each side's faces: the two arms as quads (0,1,2,3) and (0,3,4,5).
  for (const std::uint32_t* side : {front, back}) {
    triangle(mesh, side[0], side[1], side[2]);
    triangle(mesh, side[0], side[2], side[3]);
    triangle(mesh, side[0], side[3], side[4]);
    triangle(mesh, side[0], side[4], side[5]);
  }
  for (int i = 0; i < 6; ++i) {
    const int next = (i + 1) % 6;
    triangle(mesh, front[i], front[next], back[next]);
    triangle(mesh, front[i], back[next], back[i]);
    line(mesh, front[i], front[next]);
    line(mesh, back[i], back[next]);
    line(mesh, front[i], back[i]);
  }
}

}  // namespace

Mesh build(const std::string& shape, float size, float height) {
  Mesh mesh;
  const float radius = size / 2.0f;
  if (shape == "cylinder") {
    cylinder(mesh, radius, height);
  } else if (shape == "sphere") {
    sphere(mesh, radius);
  } else if (shape == "cube") {
    const float low[3] = {-radius, -radius, 0.0f};
    const float high[3] = {radius, radius, height};
    box(mesh, low, high);
  } else if (shape == "plane") {
    plane(mesh, size);
  } else if (shape == "chevron") {
    chevron(mesh, size, height);
  }
  return mesh;
}

}  // namespace kcds::runtime::markers::shapes
