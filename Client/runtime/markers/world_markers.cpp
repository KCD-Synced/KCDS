#include "world_markers.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../gizmos/gizmos.hpp"
#include "../props/pose_math.hpp"
#include "marker_shapes.hpp"

namespace kcds::runtime::markers {
namespace {

using CreateDecal = void (*)(void* engine, void* decal);
using DeleteDecalsInRange = void (*)(void* unused, const float* box, void* owner);

constexpr std::size_t kEngineCreateDecalSlot = 67;  // vtable offset 536

// The decal description the game's CreateMatDecal fills (368 bytes, zero before it is filled).
constexpr std::size_t kDecalSize = 368;
constexpr std::size_t kDecalOwnerInfo = 8;      // 64 bit, -1 for none
constexpr std::size_t kDecalPosition = 24;      // 3 floats
constexpr std::size_t kDecalNormal = 36;        // 3 floats
constexpr std::size_t kDecalSizeField = 48;     // float, the radius
constexpr std::size_t kDecalLifeTime = 52;      // float, seconds
constexpr std::size_t kDecalAngle = 56;         // float, turn around the normal
constexpr std::size_t kDecalHitDirection = 72;  // 3 floats, the way the decal is projected
constexpr std::size_t kDecalPartId = 101;       // byte, -1 for none
constexpr std::size_t kDecalMaterial = 102;     // 260 bytes, zero terminated
constexpr std::size_t kDecalMaterialCapacity = 260;

// A day: the decal stays until the server hides the marker.
constexpr float kLifeTimeSeconds = 86400.0f;
// Half the side of the box that finds the marker's decal again.
constexpr float kFindRadius = 0.05f;

// A mesh marker without a colour of its own: a translucent gold.
constexpr std::uint32_t kDefaultColor = 0xE6C34AA0;  // 0xRRGGBBAA
// A bob goes up and down once in this many seconds.
constexpr double kBobPeriodSeconds = 2.0;
constexpr double kPi = 3.14159265358979;

struct Placed {
  float position[3] = {0, 0, 0};
  bool decal = true;
  // A mesh marker: what was shown and its mesh about the position.
  kcd2s::integration::MarkerShow marker;
  shapes::Mesh mesh;
};

Natives natives;
Logger report = nullptr;
std::unordered_map<std::uint32_t, Placed> placed;

template <typename T>
void put(std::uint8_t* decal, std::size_t offset, T value) {
  std::memcpy(decal + offset, &value, sizeof(T));
}

// The marker's up axis (the rotation applied to +Z) is the surface normal of its decal.
void normal_of(const float q[4], float out[3]) {
  const float x = q[0];
  const float y = q[1];
  const float z = q[2];
  const float w = q[3];
  out[0] = 2.0f * (x * z + w * y);
  out[1] = 2.0f * (y * z - w * x);
  out[2] = 1.0f - 2.0f * (x * x + y * y);
}

// The turn around the up axis.
float angle_of(const float q[4]) {
  const float x = q[0];
  const float y = q[1];
  const float z = q[2];
  const float w = q[3];
  return std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z));
}

// 0xRRGGBBAA to the aux geometry's 0xAARRGGBB.
std::uint32_t argb_of(std::uint32_t rgba) {
  return (rgba & 0xFFu) << 24 | rgba >> 8;
}

// The edges: the colour lifted halfway to white, opaque.
std::uint32_t edge_of(std::uint32_t argb) {
  std::uint32_t edge = 0xFF000000u;
  for (int shift = 0; shift < 24; shift += 8) {
    const std::uint32_t channel = argb >> shift & 0xFFu;
    edge |= (channel + (255u - channel) / 2u) << shift;
  }
  return edge;
}

// Draws a mesh marker this frame, turned by its rotation and its spin, lifted by its bob.
void draw_mesh(const Placed& at, double now) {
  const auto& marker = at.marker;
  const double yaw = double(marker.spin) * now * kPi / 180.0;
  const float spin[4] = {0.0f, 0.0f, float(std::sin(yaw / 2.0)), float(std::cos(yaw / 2.0))};
  const float lift = marker.bob * 0.5f * float(std::sin(2.0 * kPi * now / kBobPeriodSeconds));
  std::vector<float> points(at.mesh.points.size());
  for (std::size_t i = 0; i + 2 < points.size(); i += 3) {
    float spun[3];
    float turned[3];
    pose_math::rotate(spin, &at.mesh.points[i], spun);
    pose_math::rotate(marker.rotation, spun, turned);
    points[i] = marker.position[0] + turned[0];
    points[i + 1] = marker.position[1] + turned[1];
    points[i + 2] = marker.position[2] + turned[2] + lift;
  }
  const std::uint32_t fill = argb_of(marker.color == 0 ? kDefaultColor : marker.color);
  gizmos::solid(points, at.mesh.triangles, at.mesh.lines, fill, edge_of(fill));
}

void remove_decal(const Placed& at) {
  float box[6];
  for (int i = 0; i < 3; ++i) {
    box[i] = at.position[i] - kFindRadius;
    box[3 + i] = at.position[i] + kFindRadius;
  }
  reinterpret_cast<DeleteDecalsInRange>(natives.delete_decals_in_range)(nullptr, box, nullptr);
}

}  // namespace

bool install(const Natives& found, Logger logger) {
  natives = found;
  report = logger;
  return natives.engine_3d && natives.delete_decals_in_range;
}

bool show(const kcd2s::integration::MarkerShow& marker) {
  if (marker.shape != "decal") {
    hide(marker.marker);
    Placed at;
    std::memcpy(at.position, marker.position, sizeof(at.position));
    at.decal = false;
    at.marker = marker;
    at.mesh = shapes::build(marker.shape, marker.size, marker.height);
    if (at.mesh.points.empty()) {
      return false;
    }
    placed[marker.marker] = std::move(at);
    if (report) {
      report("MARKER shown %u shape=%s size=%.2f height=%.2f", marker.marker, marker.shape.c_str(),
             double(marker.size), double(marker.height));
    }
    return true;
  }
  void* engine = natives.engine_3d ? *natives.engine_3d : nullptr;
  if (!engine || !natives.delete_decals_in_range) {
    return false;
  }
  hide(marker.marker);

  alignas(16) std::uint8_t decal[kDecalSize] = {};
  float normal[3];
  normal_of(marker.rotation, normal);
  put<std::int64_t>(decal, kDecalOwnerInfo, -1);
  std::memcpy(decal + kDecalPosition, marker.position, sizeof(marker.position));
  std::memcpy(decal + kDecalNormal, normal, sizeof(normal));
  put<float>(decal, kDecalSizeField, marker.size);
  put<float>(decal, kDecalLifeTime, kLifeTimeSeconds);
  put<float>(decal, kDecalAngle, angle_of(marker.rotation));
  const float projection[3] = {-normal[0], -normal[1], -normal[2]};
  std::memcpy(decal + kDecalHitDirection, projection, sizeof(projection));
  put<std::int8_t>(decal, kDecalPartId, -1);
  const std::size_t length = std::min(marker.material.size(), kDecalMaterialCapacity - 1);
  std::memcpy(decal + kDecalMaterial, marker.material.data(), length);

  auto** table = *reinterpret_cast<void***>(engine);
  reinterpret_cast<CreateDecal>(table[kEngineCreateDecalSlot])(engine, decal);

  Placed at;
  std::memcpy(at.position, marker.position, sizeof(at.position));
  placed[marker.marker] = at;
  if (report) {
    report("MARKER shown %u material=%s size=%.2f", marker.marker, marker.material.c_str(),
           double(marker.size));
  }
  return true;
}

bool hide(std::uint32_t marker) {
  const auto found = placed.find(marker);
  if (found == placed.end()) {
    return false;
  }
  if (found->second.decal && natives.delete_decals_in_range) {
    remove_decal(found->second);
  }
  placed.erase(found);
  if (report) {
    report("MARKER hidden %u", marker);
  }
  return true;
}

void forget_all() {
  placed.clear();
}

void frame(double now) {
  for (const auto& [id, at] : placed) {
    if (!at.decal) {
      draw_mesh(at, now);
    }
  }
}

}  // namespace kcds::runtime::markers
