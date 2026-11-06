#include "world_interactions.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>

#include "../action_hints/resource_action_hints.hpp"
#include "../physics/line_of_sight.hpp"

namespace kcds::runtime::interactions {
namespace {

using kcd2s::integration::Interaction;
using kcd2s::integration::kInteractionKeys;

// The engine hints' ids (negative: the client's own): the interactions on the aimed target show
// together, each with its key, at most this many.
constexpr std::int64_t kInteractionHints[] = {-60, -64, -65, -66};
constexpr std::size_t kHintsAtOnce = sizeof(kInteractionHints) / sizeof(kInteractionHints[0]);
// Two positions within this many metres are one target.
constexpr float kSamePlace = 0.01f;
// IEntitySystem FindEntityByName (+ 128); IEntity GetWorldTM (slot 32); the entity's render
// component (its component 0) and that component's GetLocalBounds (slot 18, min then max).
constexpr std::size_t kFindByNameSlot = 16;
constexpr std::size_t kWorldTmSlot = 32;
constexpr unsigned kRenderComponent = 0;
constexpr std::size_t kLocalBoundsSlot = 18;
// A drawn box is aimed at with this much around it, in metres; one smaller than kTinyBox on every
// side is no box (a helper entity draws nothing).
constexpr float kBoxPad = 0.05f;
constexpr float kTinyBox = 0.02f;
// A target without a drawn box: what the camera's ray hits counts as the target when it is this
// far above the target's foot (the ground under it is not the target) and at most this much
// beyond its target radius from its axis (its own parts, such as a pillory's boards).
constexpr float kTargetFloor = 0.2f;
constexpr float kOwnPartsAround = 0.3f;
// How far beyond the interaction's radius the ray looks for something solid.
constexpr float kRayBeyond = 1.0f;
// A shown hint stays this long after the camera last aimed at it, and keeps its place while
// another is at most this much nearer: an animated target (a cart's horse, held each frame) is
// missed for single frames, which made the hint blink with the game's own label.
constexpr auto kHintHold = std::chrono::milliseconds(300);
constexpr float kHintStickiness = 0.3f;

using LocalBounds = void(__fastcall*)(void* render, float* bounds);

Natives natives;
Logger log_line = nullptr;
std::vector<Interaction> interactions;
// The interaction whose hint shows first, 0 for none, the interactions shown with it, and when the
// camera last aimed at it.
std::uint32_t shown = 0;
std::vector<std::uint32_t> shown_group;
std::chrono::steady_clock::time_point shown_aimed;
std::mutex fired_mutex;
std::vector<std::uint32_t> fired;

template <class Function>
Function slot(void* object, std::size_t index) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[index]);
}

struct Ray {
  float origin[3];
  float direction[3];
};

// Where along the ray it enters the box [low, high] (0 when it starts inside); negative when it
// misses.
float enter_box(const float origin[3], const float direction[3], const float low[3],
                const float high[3]) {
  float near_t = 0;
  float far_t = 1e9f;
  for (int axis = 0; axis < 3; ++axis) {
    if (std::fabs(direction[axis]) < 1e-6f) {
      if (origin[axis] < low[axis] || origin[axis] > high[axis]) {
        return -1;
      }
      continue;
    }
    float t0 = (low[axis] - origin[axis]) / direction[axis];
    float t1 = (high[axis] - origin[axis]) / direction[axis];
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    near_t = std::max(near_t, t0);
    far_t = std::min(far_t, t1);
    if (near_t > far_t) {
      return -1;
    }
  }
  return near_t;
}

// The ray against an entity's drawn box, turned and placed as the entity is (its world matrix
// m, 3 x 4 by rows): the ray is taken into the entity's own space.
float enter_drawn_box(const Ray& ray, const float* m, const float bounds[6]) {
  // The inverse of the matrix's 3 x 3 part.
  const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
              i = m[10];
  const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::fabs(det) < 1e-8f) {
    return -1;
  }
  const float inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                        (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                        (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
  const float offset[3] = {ray.origin[0] - m[3], ray.origin[1] - m[7], ray.origin[2] - m[11]};
  float origin[3];
  float direction[3];
  for (int row = 0; row < 3; ++row) {
    origin[row] =
        inv[row * 3] * offset[0] + inv[row * 3 + 1] * offset[1] + inv[row * 3 + 2] * offset[2];
    direction[row] = inv[row * 3] * ray.direction[0] + inv[row * 3 + 1] * ray.direction[1] +
                     inv[row * 3 + 2] * ray.direction[2];
  }
  const float low[3] = {bounds[0] - kBoxPad, bounds[1] - kBoxPad, bounds[2] - kBoxPad};
  const float high[3] = {bounds[3] + kBoxPad, bounds[4] + kBoxPad, bounds[5] + kBoxPad};
  return enter_box(origin, direction, low, high);
}

// The ray against an upright cylinder standing on base.
float enter_cylinder(const Ray& ray, const float base[3], float radius, float height) {
  // Across the ground: where the ray is within the radius of the axis.
  const float ox = ray.origin[0] - base[0];
  const float oy = ray.origin[1] - base[1];
  const float dx = ray.direction[0];
  const float dy = ray.direction[1];
  const float qa = dx * dx + dy * dy;
  const float qb = 2 * (ox * dx + oy * dy);
  const float qc = ox * ox + oy * oy - radius * radius;
  float near_t = 0;
  float far_t = 1e9f;
  if (qa < 1e-8f) {
    if (qc > 0) {
      return -1;
    }
  } else {
    const float disc = qb * qb - 4 * qa * qc;
    if (disc < 0) {
      return -1;
    }
    const float root = std::sqrt(disc);
    near_t = std::max(near_t, (-qb - root) / (2 * qa));
    far_t = std::min(far_t, (-qb + root) / (2 * qa));
  }
  // Up and down: where the ray is between the cylinder's foot and top.
  const float low = base[2];
  const float high = base[2] + height;
  if (std::fabs(ray.direction[2]) < 1e-6f) {
    if (ray.origin[2] < low || ray.origin[2] > high) {
      return -1;
    }
  } else {
    float t0 = (low - ray.origin[2]) / ray.direction[2];
    float t1 = (high - ray.origin[2]) / ray.direction[2];
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    near_t = std::max(near_t, t0);
    far_t = std::min(far_t, t1);
  }
  return near_t <= far_t ? near_t : -1;
}

// The ray against a target that draws no box, standing on base: what the ray first hits must be
// the target (`hit` its distance, negative for nothing solid), or with nothing solid ahead the ray
// must pass through the target's cylinder above its foot.
float aim_at_cylinder(const Ray& ray, const float base[3], const Interaction& interaction,
                      float hit) {
  const float radius = interaction.target_radius;
  const float height = interaction.target_height;
  if (hit >= 0) {
    const float point[3] = {ray.origin[0] + ray.direction[0] * hit,
                            ray.origin[1] + ray.direction[1] * hit,
                            ray.origin[2] + ray.direction[2] * hit};
    const float dx = point[0] - base[0];
    const float dy = point[1] - base[1];
    const float around = radius + kOwnPartsAround;
    const bool beside = dx * dx + dy * dy <= around * around;
    const bool above = point[2] >= base[2] + kTargetFloor && point[2] <= base[2] + height;
    return beside && above ? hit : -1;
  }
  const float raised[3] = {base[0], base[1], base[2] + kTargetFloor};
  return enter_cylinder(ray, raised, radius, std::max(height - kTargetFloor, kTargetFloor));
}

// Where along the ray the player aims at the interaction's target; negative when they do not.
// `scene_hit` is the first solid thing the camera looks at within the longest reach (negative for
// none), cast once a frame for every interaction.
float aimed_at(const Ray& ray, const Interaction& interaction, float scene_hit) {
  // The first solid thing the camera looks at, within this interaction's reach.
  const float far = interaction.radius + kRayBeyond;
  const float hit = scene_hit >= 0 && scene_hit <= far ? scene_hit : -1;
  if (interaction.entity.empty() && interaction.body == 0) {
    const float base[3] = {interaction.position.x, interaction.position.y, interaction.position.z};
    return aim_at_cylinder(ray, base, interaction, hit);
  }
  void* entity = nullptr;
  if (interaction.body != 0) {
    // A player's or NPC's body, wherever it is now.
    entity = natives.body_of ? natives.body_of(interaction.body) : nullptr;
  } else {
    void* system = natives.entity_system ? natives.entity_system() : nullptr;
    entity = system ? slot<void*(__fastcall*)(void*, const char*)>(system, kFindByNameSlot)(
                          system, interaction.entity.c_str())
                    : nullptr;
  }
  if (!entity) {
    return -1;
  }
  const float* m = slot<const float*(__fastcall*)(void*)>(entity, kWorldTmSlot)(entity);
  using Component = void*(__fastcall*)(void* entity, unsigned id);
  void* render = natives.component
                     ? reinterpret_cast<Component>(natives.component)(entity, kRenderComponent)
                     : nullptr;
  if (render) {
    float bounds[6] = {1, 1, 1, 0, 0, 0};
    slot<LocalBounds>(render, kLocalBoundsSlot)(render, bounds);
    const bool drawn = bounds[3] - bounds[0] > kTinyBox && bounds[4] - bounds[1] > kTinyBox &&
                       bounds[5] - bounds[2] > kTinyBox;
    if (drawn) {
      // The ray enters the box, and nothing solid is well before it (the entity's own geometry
      // is inside its box).
      const float t = enter_drawn_box(ray, m, bounds);
      return t >= 0 && (hit < 0 || hit >= t - 2 * kBoxPad) ? t : -1;
    }
  }
  const float base[3] = {m[3], m[7], m[11]};
  return aim_at_cylinder(ray, base, interaction, hit);
}

void withdraw() {
  if (shown) {
    for (std::size_t k = 0; k < shown_group.size(); ++k) {
      action_hints::clear_engine_hint(kInteractionHints[k]);
    }
    if (log_line) {
      log_line("INTERACTION hint %u withdrawn", shown);
    }
    shown = 0;
    shown_group.clear();
  }
}

// Whether two interactions are on one target: the same body, the same level entity, or the same
// place.
bool same_target(const Interaction& a, const Interaction& b) {
  if (a.body != 0 || b.body != 0) {
    return a.body == b.body;
  }
  if (!a.entity.empty() || !b.entity.empty()) {
    return a.entity == b.entity;
  }
  return std::fabs(a.position.x - b.position.x) <= kSamePlace &&
         std::fabs(a.position.y - b.position.y) <= kSamePlace &&
         std::fabs(a.position.z - b.position.z) <= kSamePlace;
}

void show_one(std::size_t slot_index, const Interaction& interaction) {
  action_hints::EngineHint hint;
  hint.id = kInteractionHints[slot_index];
  hint.key = kInteractionKeys[interaction.key];
  hint.text = interaction.text;
  hint.hold = interaction.hold;
  hint.hold_seconds = interaction.hold_seconds;
  const std::uint32_t id = interaction.id;
  hint.on_fire = [id] {
    if (log_line) {
      log_line("INTERACTION used id=%u", id);
    }
    std::lock_guard<std::mutex> lock(fired_mutex);
    if (fired.size() < 16) {
      fired.push_back(id);
    }
  };
  action_hints::set_engine_hint(hint);
  if (log_line) {
    log_line("INTERACTION hint %u shown", id);
  }
}

// Shows the hints of these interactions (one target's, the first the nearest) in place of the
// ones shown.
void show(const std::vector<const Interaction*>& group) {
  std::vector<std::uint32_t> ids;
  for (const Interaction* interaction : group) {
    ids.push_back(interaction->id);
  }
  if (ids == shown_group) {
    return;
  }
  withdraw();
  for (std::size_t k = 0; k < group.size(); ++k) {
    show_one(k, *group[k]);
  }
  shown = ids.front();
  shown_group = std::move(ids);
}

}  // namespace

void install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
}

void set_list(std::vector<Interaction> list) {
  interactions = std::move(list);
  // A hint whose interaction is gone or changed goes; the next frame shows it again when it fits.
  withdraw();
  if (log_line) {
    log_line("INTERACTION list of %zu", interactions.size());
  }
}

void frame(const float* view, bool allowed) {
  if (!allowed || !view || interactions.empty()) {
    withdraw();
    return;
  }
  // The camera's position and where it looks (the matrix's forward axis, Y).
  Ray ray{{view[3], view[7], view[11]}, {view[1], view[5], view[9]}};
  const float length =
      std::sqrt(ray.direction[0] * ray.direction[0] + ray.direction[1] * ray.direction[1] +
                ray.direction[2] * ray.direction[2]);
  if (length < 1e-4f) {
    withdraw();
    return;
  }
  for (float& component : ray.direction) {
    component /= length;
  }
  const Interaction* best = nullptr;
  float best_t = 1e9f;
  const Interaction* current = nullptr;
  float current_t = -1;
  // The interactions aimed at within their reach.
  std::vector<const Interaction*> aimed;
  // One ray into the scene for all of them (up to kMaxInteractions): as far as the longest reach.
  float scene_hit = -1;
  {
    float far = 0;
    for (const Interaction& interaction : interactions) {
      far = std::max(far, interaction.radius);
    }
    far += kRayBeyond;
    const float to[3] = {ray.origin[0] + ray.direction[0] * far,
                         ray.origin[1] + ray.direction[1] * far,
                         ray.origin[2] + ray.direction[2] * far};
    float distance = 0;
    if (physics::first_hit(ray.origin, to, distance)) {
      scene_hit = distance;
    }
  }
  for (const Interaction& interaction : interactions) {
    const float t = aimed_at(ray, interaction, scene_hit);
    if (t < 0 || t > interaction.radius) {
      continue;
    }
    aimed.push_back(&interaction);
    if (interaction.id == shown) {
      current = &interaction;
      current_t = t;
    }
    if (t < best_t) {
      best = &interaction;
      best_t = t;
    }
  }
  const auto now = std::chrono::steady_clock::now();
  // The shown target keeps its place unless another is clearly nearer.
  if (current && current_t <= best_t + kHintStickiness) {
    best = current;
  }
  if (!best) {
    if (shown && now - shown_aimed < kHintHold) {
      return;
    }
    withdraw();
    return;
  }
  // Every key of the target at once, the chosen one first.
  std::vector<const Interaction*> group = {best};
  for (const Interaction* other : aimed) {
    if (other != best && group.size() < kHintsAtOnce && same_target(*best, *other)) {
      group.push_back(other);
    }
  }
  show(group);
  shown_aimed = now;
}

std::vector<std::uint32_t> take_fired() {
  std::lock_guard<std::mutex> lock(fired_mutex);
  return std::exchange(fired, {});
}

void reset() {
  withdraw();
  interactions.clear();
  std::lock_guard<std::mutex> lock(fired_mutex);
  fired.clear();
}

}  // namespace kcds::runtime::interactions
