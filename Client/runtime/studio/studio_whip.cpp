// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "studio_whip.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../core/log.hpp"
#include "../physical_props/whip_prop.hpp"
#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "studio_hold.hpp"
#include "studio_view.hpp"

namespace kcds::runtime::studio::whip {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr const char* kHandleName = "kcds_studio_whip";
// The swing test: a 0.3 m circle for the hand, 1.5 turns a second, the handle pointing out.
constexpr float kSweepCentre[3] = {32.0f, 32.0f, 33.5f};
constexpr float kSweepRadius = 0.3f;
constexpr float kSweepTurnsPerSecond = 1.5f;
constexpr float kSpawn[3] = {32.0f, 32.0f, 33.5f};

// The studio's one whip (kept for the game's life: its entity and rope must not go at the
// process's end).
physical_props::WhipProp& the_whip() {
  static auto* whip = new physical_props::WhipProp();
  return *whip;
}

bool sweeping = false;
std::mutex pending_mutex;
std::vector<std::string> pending;
// The lash's shape from the animation's lash keys at the playhead (studioLashShape), empty for
// physics.
constexpr std::size_t kMostLashPoints = 16;
std::mutex lash_shape_mutex;
std::vector<std::array<float, 3>> lash_shape;
// Whether the shape is in the character's frame (forward, right, up: studioLashShape space
// "body") or the handle's.
bool lash_shape_in_body = false;
// The character's turn (x y z w) as the last tick saw it: the body frame of a recording.
std::mutex body_mutex;
float body_turn[4] = {0.0f, 0.0f, 0.0f, 1.0f};
bool body_known = false;

double number(const sdk::Value& message, const char* name, double fallback) {
  const sdk::Value* field = integration::script_field(message, name);
  if (!field) {
    return fallback;
  }
  if (field->tag() == sdk::Tag::Double) {
    return field->as_double();
  }
  if (field->tag() == sdk::Tag::Int) {
    return double(field->as_int());
  }
  return fallback;
}

// The shortest turn from the unit vector a onto the unit vector b.
void from_to(const float a[3], const float b[3], float out[4]) {
  const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  float x = a[1] * b[2] - a[2] * b[1];
  float y = a[2] * b[0] - a[0] * b[2];
  float z = a[0] * b[1] - a[1] * b[0];
  float w = 1.0f + dot;
  if (w < 1e-4f) {
    // Opposite: half a turn about the vertical.
    x = 0;
    y = 0;
    z = 1;
    w = 0;
  }
  const float length = std::sqrt(x * x + y * y + z * z + w * w);
  out[0] = x / length;
  out[1] = y / length;
  out[2] = z / length;
  out[3] = w / length;
}

void run(const std::string& line) {
  int segments = 0;
  float length = 0;
  float pitch = 0;
  float twist = 0;
  if (std::sscanf(line.c_str(), "whip grip %f %f", &pitch, &twist) == 2) {
    the_whip().set_grip(pitch, twist);
    return;
  }
  if (line == "whip off") {
    if (the_whip().made()) {
      the_whip().remove();
      game_client::log("STUDIO whip removed");
    }
  } else if (line == "whip sweep" || line == "whip hand") {
    sweeping = line == "whip sweep";
  } else if (std::sscanf(line.c_str(), "whip %d %f", &segments, &length) == 2) {
    const bool fits = segments >= 2 && segments <= 64 && length >= 0.2f && length <= 20.0f;
    if (fits && the_whip().make(kHandleName, segments, length, kSpawn)) {
      game_client::log("STUDIO whip made: lash %d segments, %.2f m", segments, length);
    }
  }
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "studioLashShape") {
    // The lash's shape at the playhead from the animation's lash keys ([] for none: physics).
    std::vector<std::array<float, 3>> shape;
    const sdk::Value* points = integration::script_field(message, "points");
    if (points && points->tag() == sdk::Tag::Array) {
      for (const sdk::Value& point : points->as_array()) {
        if (point.tag() != sdk::Tag::Array || point.as_array().size() != 3 ||
            shape.size() >= kMostLashPoints) {
          continue;
        }
        std::array<float, 3> parts{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
          double value = 0.0;
          view::number_of(point.as_array()[axis], value);
          parts[axis] = float(std::clamp(value, -6.0, 6.0));
        }
        shape.push_back(parts);
      }
    }
    const sdk::Value* space = integration::script_field(message, "space");
    const bool in_body = space && space->tag() == sdk::Tag::Str && space->as_str() == "body";
    std::lock_guard<std::mutex> lock(lash_shape_mutex);
    lash_shape = std::move(shape);
    lash_shape_in_body = in_body;
    return true;
  }
  std::string line;
  if (operation == "studioPropGrip") {
    // How the hand holds the handle (physical_props/whip_prop.hpp set_grip).
    char text[64] = {};
    std::snprintf(text, sizeof(text), "whip grip %.1f %.1f", number(message, "pitch", 90.0),
                  number(message, "twist", 0.0));
    line = text;
  } else if (operation == "studioWhip") {
    char text[64] = {};
    std::snprintf(text, sizeof(text), "whip %d %.2f", int(number(message, "segments", 20)),
                  number(message, "length", 1.6));
    line = text;
  } else if (operation == "studioWhipSweep") {
    const sdk::Value* on = integration::script_field(message, "on");
    line = on && on->tag() == sdk::Tag::Bool && on->as_bool() ? "whip sweep" : "whip hand";
  } else if (operation == "studioWhipOff") {
    line = "whip off";
  } else {
    return false;
  }
  std::lock_guard<std::mutex> lock(pending_mutex);
  pending.push_back(line);
  return true;
}

void tick(void* local_entity) {
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lock(pending_mutex);
    lines.swap(pending);
  }
  for (const std::string& line : lines) {
    run(line);
  }
  if (!the_whip().made() || !local_entity) {
    return;
  }
  float place[3];
  float turn[4];
  if (sweeping) {
    const double angle = GetTickCount64() / 1000.0 * kSweepTurnsPerSecond * 6.283185307179586;
    const float out[3] = {float(std::cos(angle)), float(std::sin(angle)), 0.0f};
    for (int axis = 0; axis < 3; ++axis) {
      place[axis] = kSweepCentre[axis] + kSweepRadius * out[axis];
    }
    from_to(the_whip().axis(), out, turn);
  } else if (!hold::where(local_entity, place, turn)) {
    // In the hand (or the joint) the editor chose, or free in the scene (studio_hold).
    return;
  }
  // The lash stays above the floor the character stands on (its feet).
  float feet[3];
  float facing[4];
  props::entity::world_pose(local_entity, feet, facing);
  the_whip().set_ground(feet[2]);
  {
    std::lock_guard<std::mutex> lock(body_mutex);
    for (int part = 0; part < 4; ++part) {
      body_turn[part] = facing[part];
    }
    body_known = true;
  }
  // The character's body in the lash's way when it does not ignore characters (collide 1);
  // swept in a circle or free in the scene, no body holds it.
  const bool held_by_body = !sweeping && hold::in_joint();
  the_whip().set_holder(held_by_body ? local_entity : nullptr, hold::in_left_hand());
  {
    std::lock_guard<std::mutex> lock(lash_shape_mutex);
    the_whip().set_lash_shape(lash_shape, lash_shape_in_body ? facing : nullptr);
  }
  the_whip().place(place, turn);
}

std::vector<float> lash_points() {
  return the_whip().made() ? the_whip().lash_points() : std::vector<float>();
}

bool body_frame(float forward[3], float up[3]) {
  std::lock_guard<std::mutex> lock(body_mutex);
  if (!body_known) {
    return false;
  }
  const float ahead[3] = {0.0f, 1.0f, 0.0f};
  const float upward[3] = {0.0f, 0.0f, 1.0f};
  pose_math::rotate(body_turn, ahead, forward);
  pose_math::rotate(body_turn, upward, up);
  return true;
}

bool handle_frame(float along[3], float up[3]) {
  return the_whip().made() && the_whip().handle_frame(along, up);
}

bool made() {
  return the_whip().made();
}

}  // namespace kcds::runtime::studio::whip
