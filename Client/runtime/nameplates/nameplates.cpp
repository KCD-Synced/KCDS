#include "nameplates.hpp"
#include "../voice/voice_client.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

#include "../physics/line_of_sight.hpp"
#include "kcd2s/integration/nameplates.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "nameplate_paint.hpp"

namespace kcds::runtime::nameplates {
namespace {

namespace sdk = kcd2s::sdk;
namespace wire = kcd2s::integration;

// The point the plate stands on: the top of the head (from the skeleton's joints, a little
// lower where the hair is lowest over the forehead), or a height above the feet without one
// (metres). Over the head itself, not over the feet: close up and looking down a point over the
// feet falls far beside a head that leans (at the anvil). The plate is a fixed size on
// the screen a fixed gap above it, so it keeps its height over the head near and far.
constexpr float kHeadTop = -0.03f;
constexpr float kNameHeight = 1.85f;
// A body on the ground (knocked out, dead, lying down): the plate floats at least this high over
// the feet instead of lying on the body, faded while the head is below kLyingBelow.
constexpr float kLowestTop = 0.7f;
constexpr float kLyingBelow = 1.0f;
constexpr float kLyingOpacity = 0.7f;
// Up to kNearDistance metres from the camera the whole plate shows; up to kMaxDistance the name
// alone, smaller and faded (the design's "far"); past it nothing.
constexpr float kNearDistance = 30.0f;
constexpr float kMaxDistance = 60.0f;
// The plate's size by distance (never growing, with a maximum): the full
// size up to kFullSizeUntil metres, then smaller to kNearSize at kNearDistance; kFarSize past it.
// Far plates a quarter smaller than at first (too big far away).
constexpr float kFullSizeUntil = 10.0f;
constexpr float kNearSize = 0.56f;
constexpr float kFarSize = 0.45f;
// A player not kept for this long (seconds) is no longer drawn.
constexpr double kStaleAfter = 0.5;
// Points this close to the camera are not projected.
constexpr float kNearPlane = 0.1f;
// How fast the plate's height follows the head (seconds to most of the way): slow enough to
// smooth the idle sway, quick enough for crouching or sitting down.
constexpr float kHeightFollow = 0.25f;
// A bigger change than this (metres) is the body itself moving, not its sway: the plate goes
// there at once (it slid up from the legs while the world loaded).
constexpr float kHeightSnap = 0.3f;
// The plate stands over the body's centre, not over its head (over
// the head it moved all the time, leaning and hammering at the anvil). Its height stays above the
// head and steady: it rises as soon as the head is higher, and comes down only when the head stays
// lower by more than this (metres): sitting, crouching or lying, not bending over a workbench.
constexpr float kDropHold = 0.45f;
// A plate whose point is this far above the top of the screen (in screen heights) is not drawn:
// its head is out of the picture.
constexpr float kAboveScreen = 0.0f;
// A head joint this close to the body's origin is a skeleton not posed yet (while the world
// loads every joint reads at the origin): the standing height stands in for it.
constexpr float kUnposedJoint = 0.05f;
// The view is tested to this far above the plate's point, clear of the player's own head, and a
// plate goes only after it was blocked this long (a passing post or branch does not blink it).
constexpr float kSightAbove = 0.35f;
constexpr double kBlockedFor = 0.3;

// Line of sight to the plates: rays a frame, and how often one plate is looked at, in seconds.
constexpr int kSightRaysPerFrame = 16;
constexpr double kSightEveryNear = 0.15;
constexpr double kSightEveryFar = 0.5;

struct Kept {
  Seen seen;
  double at = 0;
  // The plate's height, held steady over the head (NaN until the first frame).
  float top = std::nanf("");
  // Since when the view to the plate is blocked, 0 while it is clear.
  double blocked_since = 0;
  // When the view to the plate is looked at again (a ray through the physics world each time).
  double sight_due = 0;
};

struct Label {
  std::string text;
  std::uint32_t color = 0;
  double until = 0;  // 0: until cleared
  std::uint32_t duration_ms = 0;
};

struct View {
  bool visible = true;
  bool health = true;
  bool name = true;
  bool self = false;
};

enum class RequestKind { View, Label, ClearLabels };

struct Request {
  RequestKind kind = RequestKind::View;
  View view;
  std::uint32_t entity = 0;
  Label label;
};

std::map<std::uint32_t, Kept> kept;
std::map<std::uint32_t, Label> labels;
View view;
double last_frame = 0;
std::mutex queue_mutex;
std::vector<Request> queued;

double number(const sdk::Value& message, const char* name) {
  const sdk::Value* value = wire::script_field(message, name);
  if (!value) {
    return 0;
  }
  if (value->tag() == sdk::Tag::Double) {
    return value->as_double();
  }
  if (value->tag() == sdk::Tag::Int) {
    return double(value->as_int());
  }
  return 0;
}

std::string text(const sdk::Value& message, const char* name) {
  const sdk::Value* value = wire::script_field(message, name);
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string{};
}

bool flag(const sdk::Value& message, const char* name, bool fallback) {
  const sdk::Value* value = wire::script_field(message, name);
  return value && value->tag() == sdk::Tag::Bool ? value->as_bool() : fallback;
}

std::wstring wide(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring out(std::size_t(std::max(length, 0)), L'\0');
  if (length > 0) {
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), length);
  }
  return out;
}

// Where a world point falls on the screen: x from the centre and y from the top, in screen heights;
// false behind the camera.
bool project(const float* camera, float tan_half, const float point[3], float& x, float& y) {
  const float dx = point[0] - camera[3];
  const float dy = point[1] - camera[7];
  const float dz = point[2] - camera[11];
  const float right = dx * camera[0] + dy * camera[4] + dz * camera[8];
  const float forward = dx * camera[1] + dy * camera[5] + dz * camera[9];
  const float up = dx * camera[2] + dy * camera[6] + dz * camera[10];
  if (forward <= kNearPlane || tan_half <= 0.0f) {
    return false;
  }
  const float per_metre = 0.5f / (forward * tan_half);
  x = right * per_metre;
  y = 0.5f - up * per_metre;
  return true;
}

void apply(Request& request, double now) {
  switch (request.kind) {
    case RequestKind::View:
      view = request.view;
      break;
    case RequestKind::Label:
      if (request.label.text.empty()) {
        labels.erase(request.entity);
      } else {
        request.label.until =
            request.label.duration_ms == 0 ? 0 : now + double(request.label.duration_ms) / 1000.0;
        labels[request.entity] = request.label;
      }
      break;
    case RequestKind::ClearLabels:
      labels.clear();
      break;
  }
}

}  // namespace

void keep(std::uint32_t id, const Seen& seen) {
  Kept& entry = kept[id];
  entry.seen = seen;
  entry.at = last_frame;
}

void frame(double now, const float* camera, float fov) {
  std::vector<Request> requests;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    requests.swap(queued);
  }
  for (Request& request : requests) {
    apply(request, now);
  }
  for (auto it = labels.begin(); it != labels.end();) {
    if (it->second.until != 0 && now >= it->second.until) {
      it = labels.erase(it);
    } else {
      ++it;
    }
  }
  const double previous = last_frame;
  last_frame = now;
  std::vector<Plate> plates;
  if (!camera || !view.visible) {
    publish(std::move(plates));
    return;
  }
  const float eye[3] = {camera[3], camera[7], camera[11]};
  const float tan_half = std::tan(fov / 2.0f);
  int sight_rays_left = kSightRaysPerFrame;
  for (auto it = kept.begin(); it != kept.end();) {
    if (it->second.at < previous - kStaleAfter) {
      it = kept.erase(it);
      continue;
    }
    Kept& entry = it->second;
    const Seen& seen = entry.seen;
    const std::uint32_t id = it->first;
    ++it;
    if (seen.own && !view.self) {
      continue;
    }
    wire::NameplateState tag;
    if (!wire::parse_nameplate_field(seen.field, tag) || !tag.visible) {
      continue;
    }
    // At the top of the head, followed smoothly.
    const bool posed = seen.has_head && (std::fabs(seen.head[0] - seen.feet[0]) > kUnposedJoint ||
                                         std::fabs(seen.head[1] - seen.feet[1]) > kUnposedJoint ||
                                         std::fabs(seen.head[2] - seen.feet[2]) > kUnposedJoint);
    const float head_top = posed ? seen.head[2] + kHeadTop : seen.feet[2] + kNameHeight;
    const bool lying = head_top < seen.feet[2] + kLyingBelow;
    const float top = std::max(head_top, seen.feet[2] + kLowestTop);
    if (std::isnan(entry.top) || now <= previous) {
      entry.top = top;
    } else if (top > entry.top) {
      // Never under the head: up at once by a big step, smoothly by a small one.
      if (top - entry.top > kHeightSnap) {
        entry.top = top;
      } else {
        const float follow = std::min(1.0f, float(now - previous) / kHeightFollow);
        entry.top += (top - entry.top) * follow;
      }
    } else if (entry.top - top > kDropHold) {
      // The body sat, crouched or lies: down to it, smoothly.
      const float follow = std::min(1.0f, float(now - previous) / kHeightFollow);
      entry.top += (top - entry.top) * follow;
    }
    // Over the body's centre: its feet, whatever the head does.
    const float name_at[3] = {seen.feet[0], seen.feet[1], entry.top};
    const float dx = name_at[0] - eye[0];
    const float dy = name_at[1] - eye[1];
    const float dz = name_at[2] - eye[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance > kMaxDistance) {
      continue;
    }
    if (!seen.own) {
      // The rays are shared out: a crowd's plates cost no more than these a frame, the nearest
      // are looked at more often, and a plate keeps its last answer in between.
      if (now >= entry.sight_due && sight_rays_left > 0) {
        --sight_rays_left;
        entry.sight_due = now + (distance > kNearDistance ? kSightEveryFar : kSightEveryNear);
        const float sight[3] = {name_at[0], name_at[1], name_at[2] + kSightAbove};
        if (physics::clear_between(eye, sight)) {
          entry.blocked_since = 0;
        } else if (entry.blocked_since == 0) {
          entry.blocked_since = now;
        }
      }
      if (entry.blocked_since != 0 && now - entry.blocked_since >= kBlockedFor) {
        continue;
      }
    }
    Plate plate;
    if (!project(camera, tan_half, name_at, plate.x, plate.y) || plate.y < kAboveScreen) {
      continue;
    }
    plate.id = id;
    plate.distance = distance;
    plate.distant = distance > kNearDistance;
    plate.opacity = lying ? kLyingOpacity : 1.0f;
    if (plate.distant) {
      plate.size = kFarSize;
    } else if (distance > kFullSizeUntil) {
      const float along = (distance - kFullSizeUntil) / (kNearDistance - kFullSizeUntil);
      plate.size = 1.0f - (1.0f - kNearSize) * along;
    }
    const std::string& shown = tag.text.empty() ? seen.nickname : tag.text;
    if (view.name && tag.name_visible) {
      plate.name = wide(shown);
      plate.title = wide(tag.title);
      plate.title_color = tag.title_color;
      plate.title_size = float(tag.title_size);
    }
    plate.color = tag.color;
    if (view.health && tag.health_visible && seen.health >= 0.0f) {
      plate.health = std::clamp(seen.health / 100.0f, 0.0f, 1.0f);
    }
    plate.speaking = seen.speaking;
    plate.voice_talking =
        kcds::runtime::voice::talking_icons_shown() && kcds::runtime::voice::is_talking(plate.id);
    const auto label = labels.find(id);
    if (label != labels.end()) {
      plate.label = wide(label->second.text);
      plate.label_color = label->second.color == 0 ? tag.color : label->second.color;
    }
    if (plate.name.empty() && plate.health < 0.0f && plate.label.empty()) {
      continue;
    }
    plates.push_back(std::move(plate));
  }
  publish(std::move(plates));
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = wire::script_operation(message);
  Request request;
  if (operation == "nameplateView") {
    request.kind = RequestKind::View;
    request.view.visible = flag(message, "visible", true);
    request.view.health = flag(message, "health", true);
    request.view.name = flag(message, "name", true);
    request.view.self = flag(message, "self", false);
  } else if (operation == "nameplateLabel") {
    const double entity = number(message, "entity");
    const double duration = number(message, "durationMs");
    if (!(entity >= 1 && entity <= 4294967295.0)) {
      return true;
    }
    request.kind = RequestKind::Label;
    request.entity = std::uint32_t(entity);
    request.label.text = text(message, "text");
    request.label.color = std::uint32_t(std::clamp(number(message, "color"), 0.0, 4294967295.0));
    request.label.duration_ms = std::uint32_t(std::clamp(duration, 0.0, 3600000.0));
  } else if (operation == "nameplateLabelsClear") {
    request.kind = RequestKind::ClearLabels;
  } else {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  queued.push_back(std::move(request));
  return true;
}

void reset() {
  kept.clear();
  labels.clear();
  publish({});
}

}  // namespace kcds::runtime::nameplates
