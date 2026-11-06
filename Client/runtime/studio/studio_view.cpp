#include "studio_view.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>

#include "../camera/anchor_view.hpp"
#include "../gizmos/gizmos.hpp"
#include "../noclip/free_camera.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::studio::view {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr double kPi = 3.14159265358979323846;
constexpr int kRingSegments = 40;
// A press this near a ring's line (metres, at the ring) takes the ring.
constexpr float kRingReach = 0.02f;
constexpr float kRingThickness = 2.5f;
constexpr float kActiveRingThickness = 5.0f;

std::mutex viewport_mutex;
// The editor's view of the world on the screen, -1 to 1 from the left and the bottom.
Rect viewport;

// A point in the camera's space: across (right), depth (forward), up.
void camera_space(const View& view, const float* point, double out[3]) {
  const double offset[3] = {point[0] - view.eye[0], point[1] - view.eye[1], point[2] - view.eye[2]};
  const double* axes[3] = {view.right, view.forward, view.up};
  for (int axis = 0; axis < 3; ++axis) {
    out[axis] = axes[axis][0] * offset[0] + axes[axis][1] * offset[1] + axes[axis][2] * offset[2];
  }
}

// The five sides of what the editor's view shows, each a value that is not negative inside:
// in front of the camera, and within the rectangle's left, right, bottom and top.
void sides(const View& view, const double p[3], double out[5]) {
  out[0] = p[1] - kNear;
  out[1] = p[0] - view.rect.left * view.tan_h * p[1];
  out[2] = view.rect.right * view.tan_h * p[1] - p[0];
  out[3] = p[2] - view.rect.bottom * view.tan_v * p[1];
  out[4] = view.rect.top * view.tan_v * p[1] - p[2];
}

}  // namespace

const float kAxisColours[3][4] = {
    {0.94f, 0.42f, 0.37f, 1.0f}, {0.42f, 0.77f, 0.42f, 1.0f}, {0.37f, 0.64f, 0.95f, 1.0f}};

void set_viewport(const Rect& rect) {
  std::lock_guard<std::mutex> lock(viewport_mutex);
  viewport = rect;
}

// The camera as noclip's toScreen works it out (Client/resources/noclip.cjs basis).
bool current(View& view) {
  camera::Pose pose;
  double fov = 0;
  double aspect = 0;
  if (!noclip::camera_view(pose, fov, aspect) || fov <= 0) {
    return false;
  }
  const double yaw = pose.yaw * kPi / 180;
  const double pitch = pose.pitch * kPi / 180;
  view.eye[0] = pose.x;
  view.eye[1] = pose.y;
  view.eye[2] = pose.z;
  view.forward[0] = -std::sin(yaw) * std::cos(pitch);
  view.forward[1] = std::cos(yaw) * std::cos(pitch);
  view.forward[2] = std::sin(pitch);
  view.right[0] = std::cos(yaw);
  view.right[1] = std::sin(yaw);
  view.right[2] = 0;
  view.up[0] = std::sin(yaw) * std::sin(pitch);
  view.up[1] = -std::cos(yaw) * std::sin(pitch);
  view.up[2] = std::cos(pitch);
  view.tan_v = std::tan(fov * kPi / 360);
  view.tan_h = view.tan_v * (aspect > 0 ? aspect : 16.0 / 9.0);
  std::lock_guard<std::mutex> lock(viewport_mutex);
  view.rect = viewport;
  return true;
}

bool inside(const View& view, const float* point) {
  double p[3];
  camera_space(view, point, p);
  double values[5];
  sides(view, p, values);
  return std::all_of(std::begin(values), std::end(values), [](double v) { return v >= 0; });
}

// Liang-Barsky against the five sides, which are linear along the line.
void clipped_line(const View& view, const float* a, const float* b, const float* colour,
                  float thickness, bool over_scene) {
  double pa[3];
  double pb[3];
  camera_space(view, a, pa);
  camera_space(view, b, pb);
  double va[5];
  double vb[5];
  sides(view, pa, va);
  sides(view, pb, vb);
  double enter = 0;
  double leave = 1;
  for (int side = 0; side < 5; ++side) {
    if (va[side] < 0 && vb[side] < 0) {
      return;
    }
    if (va[side] < 0) {
      enter = std::max(enter, va[side] / (va[side] - vb[side]));
    } else if (vb[side] < 0) {
      leave = std::min(leave, va[side] / (va[side] - vb[side]));
    }
  }
  if (enter >= leave) {
    return;
  }
  float from[3];
  float to[3];
  for (int axis = 0; axis < 3; ++axis) {
    from[axis] = float(a[axis] + (b[axis] - a[axis]) * enter);
    to[axis] = float(a[axis] + (b[axis] - a[axis]) * leave);
  }
  if (over_scene) {
    gizmos::line(from, to, colour, thickness);
  } else {
    gizmos::line(from, to, colour);
  }
}

bool number_of(const sdk::Value& value, double& out) {
  if (value.tag() == sdk::Tag::Int) {
    out = double(value.as_int());
    return true;
  }
  if (value.tag() == sdk::Tag::Double) {
    out = value.as_double();
    return true;
  }
  return false;
}

bool numbers_of(const sdk::Value& message, const char* name, double* out, std::size_t size) {
  const sdk::Value* field = integration::script_field(message, name);
  if (!field || field->tag() != sdk::Tag::Array || field->as_array().size() != size) {
    return false;
  }
  for (std::size_t index = 0; index < size; ++index) {
    if (!number_of(field->as_array()[index], out[index])) {
      return false;
    }
  }
  return true;
}

bool read_ray(const sdk::Value& message, Ray& ray) {
  if (!numbers_of(message, "from", ray.from, 3) ||
      !numbers_of(message, "direction", ray.direction, 3)) {
    return false;
  }
  const double length =
      std::sqrt(ray.direction[0] * ray.direction[0] + ray.direction[1] * ray.direction[1] +
                ray.direction[2] * ray.direction[2]);
  if (length <= 0) {
    return false;
  }
  for (double& part : ray.direction) {
    part /= length;
  }
  return true;
}

void turn_vector(const float q[4], const float v[3], float out[3]) {
  // q v q*: the vector turned by the quaternion (x, y, z, w).
  const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
  const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
  const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
  out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
  out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
  out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

Rings rings_about(const float centre[3], const float turn[4], float radius) {
  Rings rings;
  rings.radius = radius;
  for (int axis = 0; axis < 3; ++axis) {
    rings.centre[axis] = centre[axis];
  }
  const float units[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int ring = 0; ring < 3; ++ring) {
    turn_vector(turn, units[ring], rings.normal[ring]);
    turn_vector(turn, units[(ring + 1) % 3], rings.across[ring]);
    turn_vector(turn, units[(ring + 2) % 3], rings.up[ring]);
  }
  return rings;
}

void draw_rings(const View& view, const Rings& rings, int active) {
  for (int ring = 0; ring < 3; ++ring) {
    const float thickness = ring == active ? kActiveRingThickness : kRingThickness;
    float previous[3];
    for (int step = 0; step <= kRingSegments; ++step) {
      const float angle = float(step) / kRingSegments * 6.2831853f;
      float point[3];
      for (int axis = 0; axis < 3; ++axis) {
        point[axis] =
            rings.centre[axis] + rings.radius * (std::cos(angle) * rings.across[ring][axis] +
                                                 std::sin(angle) * rings.up[ring][axis]);
      }
      if (step > 0) {
        clipped_line(view, previous, point, kAxisColours[ring], thickness);
      }
      std::copy(std::begin(point), std::end(point), std::begin(previous));
    }
  }
}

int ring_hit(const Rings& rings, const Ray& ray) {
  int best = -1;
  double best_along = 1e9;
  for (int ring = 0; ring < 3; ++ring) {
    const float* n = rings.normal[ring];
    const double facing =
        ray.direction[0] * n[0] + ray.direction[1] * n[1] + ray.direction[2] * n[2];
    if (std::fabs(facing) < 1e-4) {
      continue;
    }
    const double along =
        ((rings.centre[0] - ray.from[0]) * n[0] + (rings.centre[1] - ray.from[1]) * n[1] +
         (rings.centre[2] - ray.from[2]) * n[2]) /
        facing;
    if (along <= kNear) {
      continue;
    }
    double distance = 0;
    for (int axis = 0; axis < 3; ++axis) {
      const double met = ray.from[axis] + ray.direction[axis] * along;
      distance += (met - rings.centre[axis]) * (met - rings.centre[axis]);
    }
    // Reach grows with the distance, as a ring far away is thin on the screen.
    const double reach = kRingReach * std::max(1.0, along / 2.0);
    if (std::fabs(std::sqrt(distance) - rings.radius) <= reach && along < best_along) {
      best = ring;
      best_along = along;
    }
  }
  return best;
}

}  // namespace kcds::runtime::studio::view
