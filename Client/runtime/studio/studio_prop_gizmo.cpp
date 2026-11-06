#include "studio_prop_gizmo.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "kcd2s/integration/script_channel.hpp"
#include "studio_hold.hpp"
#include "studio_mesh.hpp"
#include "studio_view.hpp"

namespace kcds::runtime::studio::prop_gizmo {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

enum Thing { kNone = 0, kProp = 1, kMesh = 2, kEnd = 3 };
enum Tool { kMove = 0, kRotate = 1, kScale = 2 };

constexpr float kArrowLength = 0.35f;
constexpr float kHeadLength = 0.06f;
constexpr float kScaleBox = 0.025f;
constexpr float kRingRadius = 0.25f;
constexpr float kThickness = 3.0f;
constexpr float kActiveThickness = 6.0f;
// A press this near a handle's line (metres at 2 m away; more farther off) takes the handle.
constexpr double kHandleReach = 0.03;
// A press within this angle (radians) of the held prop's place picks the prop.
constexpr double kPropAngle = 0.06;
constexpr float kUnits[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
// The rings turn about the world's axes.
constexpr float kLevel[4] = {0, 0, 0, 1};

std::atomic<int> thing{kNone};
std::atomic<int> tool{kMove};
std::atomic<int> active_axis{-1};
// Whether a rope or a whip is made (it can be picked though the gizmo is elsewhere).
std::atomic<bool> prop_made{false};
// The rope's pinned end (the editor's End X, Y, Z), when it is pinned: picked and moved too.
std::mutex end_mutex;
bool end_shown = false;
float end_place[3] = {0, 0, 0};

std::mutex pick_mutex;
std::optional<view::Ray> pick;

// What is chosen is marked in this colour, so you see what the gizmo moves.
constexpr float kMarkColour[4] = {1.0f, 0.62f, 0.12f, 1.0f};
constexpr float kMarkSize = 0.14f;
constexpr float kMarkThickness = 4.0f;

const char* thing_name(int which) {
  if (which == kProp) {
    return "prop";
  }
  if (which == kEnd) {
    return "end";
  }
  return which == kMesh ? "mesh" : "";
}

// The mark round a point: three short lines across it, one along each axis.
void draw_point_mark(const view::View& shown, const float centre[3]) {
  for (int axis = 0; axis < 3; ++axis) {
    float from[3];
    float to[3];
    for (int part = 0; part < 3; ++part) {
      from[part] = centre[part] - kMarkSize * kUnits[axis][part];
      to[part] = centre[part] + kMarkSize * kUnits[axis][part];
    }
    view::clipped_line(shown, from, to, kMarkColour, kMarkThickness);
  }
}

// The mark round a box: its twelve edges.
void draw_box_mark(const view::View& shown, const float min[3], const float max[3]) {
  float corners[8][3];
  for (int corner = 0; corner < 8; ++corner) {
    corners[corner][0] = (corner & 1) ? max[0] : min[0];
    corners[corner][1] = (corner & 2) ? max[1] : min[1];
    corners[corner][2] = (corner & 4) ? max[2] : min[2];
  }
  for (int corner = 0; corner < 8; ++corner) {
    for (const int bit : {1, 2, 4}) {
      if ((corner & bit) == 0) {
        view::clipped_line(shown, corners[corner], corners[corner | bit], kMarkColour,
                           kMarkThickness);
      }
    }
  }
}

double dot(const double a[3], const double b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// How near the press's line passes the segment from a to b (metres), and how far along the line
// that is (`along`).
double ray_to_segment(const view::Ray& ray, const float a[3], const float b[3], double& along) {
  const double v[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const double w[3] = {ray.from[0] - a[0], ray.from[1] - a[1], ray.from[2] - a[2]};
  const double uv = dot(ray.direction, v);
  const double vv = dot(v, v);
  const double uw = dot(ray.direction, w);
  const double vw = dot(v, w);
  const double denominator = vv - uv * uv;
  double t = denominator > 1e-9 ? (vw - uv * uw) / denominator : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  along = std::max(0.0, t * uv - uw);
  double distance = 0;
  for (int axis = 0; axis < 3; ++axis) {
    const double on_ray = ray.from[axis] + ray.direction[axis] * along;
    const double on_segment = a[axis] + v[axis] * t;
    distance += (on_ray - on_segment) * (on_ray - on_segment);
  }
  return std::sqrt(distance);
}

// Where the press's line enters the box (metres along it), or -1 when it misses.
double ray_to_box(const view::Ray& ray, const float min[3], const float max[3]) {
  double enter = 0;
  double leave = 1e9;
  for (int axis = 0; axis < 3; ++axis) {
    if (std::fabs(ray.direction[axis]) < 1e-9) {
      if (ray.from[axis] < min[axis] || ray.from[axis] > max[axis]) {
        return -1;
      }
      continue;
    }
    double near_side = (min[axis] - ray.from[axis]) / ray.direction[axis];
    double far_side = (max[axis] - ray.from[axis]) / ray.direction[axis];
    if (near_side > far_side) {
      std::swap(near_side, far_side);
    }
    enter = std::max(enter, near_side);
    leave = std::min(leave, far_side);
  }
  return enter <= leave ? enter : -1;
}

// The angle between the press's line and the point, and how far along it the point is.
double ray_to_point(const view::Ray& ray, const float point[3], double& along) {
  const double offset[3] = {point[0] - ray.from[0], point[1] - ray.from[1], point[2] - ray.from[2]};
  along = dot(offset, ray.direction);
  const double length = std::sqrt(dot(offset, offset));
  if (along <= view::kNear || length <= 0) {
    return 1e9;
  }
  return std::acos(std::clamp(along / length, -1.0, 1.0));
}

// The handle of the move or scale gizmo the press takes (0 X, 1 Y, 2 Z), or -1.
int arrow_hit(const view::Ray& ray, const float centre[3]) {
  int best = -1;
  double best_along = 1e9;
  for (int axis = 0; axis < 3; ++axis) {
    float tip[3];
    for (int part = 0; part < 3; ++part) {
      tip[part] = centre[part] + kArrowLength * kUnits[axis][part];
    }
    double along = 0;
    const double distance = ray_to_segment(ray, centre, tip, along);
    const double reach = kHandleReach * std::max(1.0, along / 2.0);
    if (along > view::kNear && distance <= reach && along < best_along) {
      best = axis;
      best_along = along;
    }
  }
  return best;
}

void draw_arrows(const view::View& shown, const float centre[3], bool boxes, int active) {
  for (int axis = 0; axis < 3; ++axis) {
    const float thickness = axis == active ? kActiveThickness : kThickness;
    const float* colour = view::kAxisColours[axis];
    const float* along = kUnits[axis];
    const float* side = kUnits[axis == 2 ? 0 : 2];
    const float* other = kUnits[3 - axis - (axis == 2 ? 0 : 2)];
    float tip[3];
    for (int part = 0; part < 3; ++part) {
      tip[part] = centre[part] + kArrowLength * along[part];
    }
    view::clipped_line(shown, centre, tip, colour, thickness);
    if (boxes) {
      // The scale handle: a small square round the tip.
      float corners[4][3];
      const float signs[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
      for (int corner = 0; corner < 4; ++corner) {
        for (int part = 0; part < 3; ++part) {
          corners[corner][part] = tip[part] + kScaleBox * (signs[corner][0] * side[part] +
                                                           signs[corner][1] * other[part]);
        }
      }
      for (int corner = 0; corner < 4; ++corner) {
        view::clipped_line(shown, corners[corner], corners[(corner + 1) % 4], colour, thickness);
      }
      continue;
    }
    // The arrow's head: two short lines back from the tip.
    for (const float sign : {1.0f, -1.0f}) {
      float back[3];
      for (int part = 0; part < 3; ++part) {
        back[part] = tip[part] - kHeadLength * along[part] + sign * kHeadLength * 0.5f * side[part];
      }
      view::clipped_line(shown, tip, back, colour, thickness);
    }
  }
}

sdk::Value numbers_value(const float v[3]) {
  auto rounded = [](float x) { return sdk::Value(std::round(double(x) * 1000.0) / 1000.0); };
  return sdk::Value(sdk::Value::Array{rounded(v[0]), rounded(v[1]), rounded(v[2])});
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "studioPickProp") {
    view::Ray ray;
    if (view::read_ray(message, ray)) {
      std::lock_guard<std::mutex> lock(pick_mutex);
      pick = ray;
    }
    return true;
  }
  if (operation != "studioPropGizmo") {
    return false;
  }
  const sdk::Value* which = integration::script_field(message, "thing");
  const std::string name = which && which->tag() == sdk::Tag::Str ? which->as_str() : "";
  int chosen = kNone;
  if (name == "prop") {
    chosen = kProp;
  } else if (name == "mesh") {
    chosen = kMesh;
  } else if (name == "end") {
    chosen = kEnd;
  }
  thing.store(chosen);
  {
    double end[3];
    std::lock_guard<std::mutex> lock(end_mutex);
    end_shown = view::numbers_of(message, "end", end, 3);
    for (int part = 0; part < 3 && end_shown; ++part) {
      end_place[part] = float(end[part]);
    }
  }
  const sdk::Value* given_tool = integration::script_field(message, "tool");
  const std::string tool_name =
      given_tool && given_tool->tag() == sdk::Tag::Str ? given_tool->as_str() : "move";
  tool.store(tool_name == "rotate" ? kRotate : tool_name == "scale" ? kScale : kMove);
  const sdk::Value* axis = integration::script_field(message, "axis");
  double axis_number = -1;
  if (!axis || !view::number_of(*axis, axis_number)) {
    axis_number = -1;
  }
  active_axis.store(int(axis_number));
  const sdk::Value* made = integration::script_field(message, "prop");
  prop_made.store(made && made->tag() == sdk::Tag::Bool && made->as_bool());
  double rect[4];
  if (view::numbers_of(message, "view", rect, 4)) {
    view::set_viewport(view::Rect{rect[0], rect[1], rect[2], rect[3]});
  }
  return true;
}

void tick(void* local_entity, const std::function<bool(const sdk::Value&)>& send) {
  std::optional<view::Ray> ray;
  {
    std::lock_guard<std::mutex> lock(pick_mutex);
    ray.swap(pick);
  }
  const int on = thing.load();
  if (on == kNone && !ray) {
    return;
  }
  float prop_place[3];
  float prop_turn[4];
  const bool with_prop = prop_made.load() && hold::where(local_entity, prop_place, prop_turn);
  float mesh_place[3];
  float mesh_turn[4];
  float mesh_min[3];
  float mesh_max[3];
  const bool with_mesh = mesh::where(mesh_place, mesh_turn, mesh_min, mesh_max);
  float pinned_end[3];
  bool with_end = false;
  {
    std::lock_guard<std::mutex> lock(end_mutex);
    with_end = end_shown && prop_made.load();
    std::copy(end_place, end_place + 3, pinned_end);
  }
  const float* centre = nullptr;
  if (on == kProp && with_prop) {
    centre = prop_place;
  } else if (on == kMesh && with_mesh) {
    centre = mesh_place;
  } else if (on == kEnd && with_end) {
    centre = pinned_end;
  }
  // The rope's end only moves: its gizmo is the move arrows whatever the tool.
  const int shown_tool = on == kEnd ? kMove : tool.load();
  if (ray) {
    // The gizmo's handles first, then the nearest thing the line meets.
    int picked = kNone;
    int axis = -1;
    if (centre) {
      if (shown_tool == kRotate) {
        axis = view::ring_hit(view::rings_about(centre, kLevel, kRingRadius), *ray);
      } else {
        axis = arrow_hit(*ray, centre);
      }
      picked = axis >= 0 ? on : kNone;
    }
    if (picked == kNone) {
      double best_along = 1e9;
      if (with_mesh) {
        const double along = ray_to_box(*ray, mesh_min, mesh_max);
        if (along >= 0 && along < best_along) {
          best_along = along;
          picked = kMesh;
        }
      }
      double along = 0;
      if (with_prop && ray_to_point(*ray, prop_place, along) <= kPropAngle && along < best_along) {
        best_along = along;
        picked = kProp;
      }
      if (with_end && ray_to_point(*ray, pinned_end, along) <= kPropAngle && along < best_along) {
        picked = kEnd;
      }
    }
    float at[3] = {0, 0, 0};
    float degrees[3] = {0, 0, 0};
    if (picked == kProp) {
      std::copy(prop_place, prop_place + 3, at);
      hold::degrees_of_turn(prop_turn, degrees);
    } else if (picked == kEnd) {
      std::copy(pinned_end, pinned_end + 3, at);
    } else if (picked == kMesh) {
      std::copy(mesh_place, mesh_place + 3, at);
      hold::degrees_of_turn(mesh_turn, degrees);
    }
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioPropPicked")},
                                    {"thing", sdk::Value(std::string(thing_name(picked)))},
                                    {"axis", sdk::Value(std::int64_t(axis))},
                                    {"at", numbers_value(at)},
                                    {"turn", numbers_value(degrees)}}));
  }
  view::View shown;
  if (!centre || !view::current(shown)) {
    return;
  }
  // What is chosen, marked: the mesh's box, or a cross on the prop's start or the rope's end.
  if (on == kMesh) {
    draw_box_mark(shown, mesh_min, mesh_max);
  } else {
    draw_point_mark(shown, centre);
  }
  const int active = active_axis.load();
  if (shown_tool == kRotate) {
    view::draw_rings(shown, view::rings_about(centre, kLevel, kRingRadius), active);
  } else {
    draw_arrows(shown, centre, shown_tool == kScale, active);
  }
}

}  // namespace kcds::runtime::studio::prop_gizmo
