#include "../marker_shapes.hpp"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using kcds::runtime::markers::shapes::build;
  // A ring 2 m across with a 10 cm band: two rings of 24 points, 48 faces between them, both
  // edges outlined, all of it flat and just over the ground.
  const auto ring = build("ring", 2.0f, 0.1f);
  expect(ring.points.size() == 48 * 3, "two rings of points");
  expect(ring.triangles.size() == 48 * 3, "the band's faces");
  expect(ring.lines.size() == 48 * 2, "both edges outlined");
  bool flat = true;
  float outer = 0.0f;
  float inner = 10.0f;
  for (std::size_t i = 0; i < ring.points.size(); i += 3) {
    flat = flat && std::fabs(ring.points[i + 2] - ring.points[2]) < 1e-6f;
    const float r = std::hypot(ring.points[i], ring.points[i + 1]);
    outer = std::fmax(outer, r);
    inner = std::fmin(inner, r);
  }
  expect(flat && ring.points[2] > 0.0f, "flat, lifted over the ground");
  expect(std::fabs(outer - 1.0f) < 1e-4f, "its outer edge is half its size");
  expect(std::fabs(inner - 0.9f) < 1e-4f, "its inner edge is the band width in");
  // A band wider than the ring fills the disc: no inner outline.
  const auto disc = build("ring", 1.0f, 5.0f);
  expect(disc.lines.size() == 24 * 2, "a full disc has only its outer edge");
  expect(build("torus", 1.0f, 1.0f).points.empty(), "an unknown shape is empty");
  if (failures == 0) {
    std::printf("marker_shapes: all passed\n");
  }
  return failures == 0 ? 0 : 1;
}
