#include "siege_engines.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "../action_hints/resource_action_hints.hpp"
#include "../physics/line_of_sight.hpp"
#include "../props/prop_entity.hpp"

namespace kcds::runtime::siege {
namespace {

namespace integration = kcd2s::integration;
using integration::SiegeAction;
using integration::SiegeCycle;
using integration::SiegeEngineKind;
using integration::SiegeEngineOrder;
using integration::SiegeEngineState;
using integration::SiegeOrderKind;
using integration::SiegeShot;

namespace entity = props::entity;

constexpr const char* kTrebuchetModel = "objects/characters/assets/trebuchet/trebuchet.cdf";
constexpr const char* kCannonModel = "objects/manmade/task_specific_props/combat/cannon.cgf";
constexpr const char* kCannonWreckModel =
    "objects/manmade/task_specific_props/combat/cannon_broken.cgf";
constexpr const char* kTrebuchetStone = "objects/characters/assets/trebuchet/trebuchet_stone.cgf";
constexpr const char* kCannonBall = "objects/manmade/task_specific_props/combat/cannon_ammo.cgf";
// The trebuchet's clips (Animations/Mannequin/ADB/kcd_trebuchet.adb).
constexpr const char* kDrawClip = "quest_trebuchet_pull_back";
constexpr const char* kLoadClip = "quest_trebuchet_load";
constexpr const char* kFireClip = "quest_trebuchet_fire";
constexpr float kClipBlend = 0.2f;

// The server lets a player this close take an empty engine (siege/engines.cjs USE_DISTANCE);
// the hint asks a little nearer, so the ask is not refused at the edge.
constexpr float kUseDistance = 2.8f;
// A player who works an engine and walks this far from it leaves it.
constexpr float kLeaveDistance = 6.0f;
// The game's "use" control (action_hints' key).
constexpr const char* kUseKey = "primary_action";
constexpr std::int64_t kSiegeHint = -61;
constexpr float kGravity = 9.81f;
// A stone the world did not stop by then is at its target.
constexpr double kFlightSlackSeconds = 0.5;
// Stones and asks held at most, so a flood of orders cannot grow them.
constexpr std::size_t kMaxStones = 64;
constexpr std::size_t kMaxReports = 32;

struct Engine {
  SiegeEngineKind kind = SiegeEngineKind::Trebuchet;
  float position[3]{};
  SiegeEngineState state;
  bool built_wrecked = false;
  // A leave the player's walk away already asked for.
  bool leaving = false;
};

struct Stone {
  SiegeShot shot;
  double start = 0;
  float rise = 0;  // vertical speed at the launch, metres per second
  float last[3]{};
};

enum class Offer { None, Operate, Load, Fire };

Logger log_line = nullptr;
// Engine thread only: server engine id -> what it shows.
std::unordered_map<std::uint32_t, Engine> engines;
// Engine thread only: shot number -> its stone in the air.
std::unordered_map<std::uint32_t, Stone> stones;
std::uint32_t offered_engine = 0;
Offer offered = Offer::None;
// Where the camera looked when the hint was last offered: the fire goes there.
float camera_forward[3]{};
bool camera_forward_valid = false;

std::mutex reports_mutex;
std::vector<integration::SiegeUse> uses;
std::vector<integration::SiegeImpact> impacts;

std::string engine_name(std::uint32_t id) {
  return "kcds_siege_" + std::to_string(id);
}

std::string stone_name(std::uint32_t shot) {
  return "kcds_siege_shot_" + std::to_string(shot);
}

void queue_use(std::uint32_t engine, SiegeAction action) {
  integration::SiegeUse use;
  use.engine = engine;
  use.action = action;
  if (action == SiegeAction::Fire && camera_forward_valid) {
    use.has_look = true;
    for (std::size_t i = 0; i < 3; ++i) {
      use.look[i] = camera_forward[i];
    }
  }
  if (log_line) {
    log_line("SIEGE engine %u: the player asks %u", engine, unsigned(action));
  }
  std::lock_guard<std::mutex> lock(reports_mutex);
  if (uses.size() < kMaxReports) {
    uses.push_back(use);
  }
}

void queue_impact(const SiegeShot& shot, const float position[3]) {
  integration::SiegeImpact impact;
  impact.engine = shot.engine;
  impact.shot = shot.shot;
  for (std::size_t i = 0; i < 3; ++i) {
    impact.position[i] = position[i];
  }
  std::lock_guard<std::mutex> lock(reports_mutex);
  if (impacts.size() < kMaxReports) {
    impacts.push_back(impact);
  }
}

const char* model_of(const Engine& engine) {
  if (engine.kind == SiegeEngineKind::Cannon) {
    return engine.state.wrecked ? kCannonWreckModel : kCannonModel;
  }
  return kTrebuchetModel;
}

// The trebuchet's clip for a step of the cycle; none for a step that holds what played before.
const char* clip_of(SiegeCycle cycle) {
  switch (cycle) {
    case SiegeCycle::Drawing:
      return kDrawClip;
    case SiegeCycle::Loading:
      return kLoadClip;
    case SiegeCycle::Firing:
      return kFireClip;
    default:
      return nullptr;
  }
}

void play_cycle(std::uint32_t id, const Engine& engine, const char* clip) {
  void* body = entity::find(engine_name(id));
  if (!body || engine.kind != SiegeEngineKind::Trebuchet) {
    return;
  }
  if (engine.state.wrecked) {
    entity::stop(body);
    return;
  }
  if (!clip) {
    return;
  }
  entity::Clip played;
  played.name = clip;
  played.blend = kClipBlend;
  played.speed = engine.state.speed;
  played.loop = false;
  if (!entity::play(body, played) && log_line) {
    log_line("SIEGE engine %u: no clip %s", id, clip);
  }
}

// Removes the engine's entity when it has one, then builds it from what the engine shows.
bool build(std::uint32_t id, Engine& engine) {
  const std::string name = engine_name(id);
  entity::remove(name);
  void* body = entity::spawn_mesh(name, engine.position, model_of(engine), std::string());
  if (!body) {
    return false;
  }
  entity::rotate(body, engine.state.rotation.data());
  engine.built_wrecked = engine.state.wrecked;
  return true;
}

void spawn(const SiegeEngineOrder& order) {
  Engine engine;
  engine.kind = order.engine;
  for (std::size_t i = 0; i < 3; ++i) {
    engine.position[i] = order.position[i];
  }
  engine.state = order.state;
  const bool built = build(order.id, engine);
  if (log_line) {
    log_line("SIEGE engine %u (%s): %s", order.id,
             engine.kind == SiegeEngineKind::Cannon ? "cannon" : "trebuchet",
             built ? "spawned" : "not spawned");
  }
  if (!built) {
    engines.erase(order.id);
    return;
  }
  // A late joiner sees the step the engine is in; ready holds the load's last key.
  const SiegeCycle cycle = engine.state.cycle;
  play_cycle(order.id, engine, cycle == SiegeCycle::Ready ? kLoadClip : clip_of(cycle));
  engines[order.id] = engine;
}

void update(const SiegeEngineOrder& order) {
  const auto found = engines.find(order.id);
  if (found == engines.end()) {
    return;
  }
  Engine& engine = found->second;
  const SiegeEngineState before = engine.state;
  engine.state = order.state;
  if (!engine.state.mine) {
    engine.leaving = false;
  }
  if (engine.kind == SiegeEngineKind::Cannon && engine.built_wrecked != engine.state.wrecked) {
    if (!build(order.id, engine)) {
      engines.erase(found);
    }
    return;
  }
  if (void* body = entity::find(engine_name(order.id))) {
    entity::rotate(body, engine.state.rotation.data());
  }
  if (before.wrecked != engine.state.wrecked) {
    play_cycle(order.id, engine, nullptr);
  } else if (before.cycle != engine.state.cycle || before.speed != engine.state.speed) {
    play_cycle(order.id, engine, clip_of(engine.state.cycle));
  }
}

void destroy(std::uint32_t id) {
  entity::remove(engine_name(id));
  engines.erase(id);
}

// Where the stone is `t` seconds after it was let go.
void stone_at(const Stone& stone, double t, float out[3]) {
  const SiegeShot& shot = stone.shot;
  const double share = shot.flight > 0 ? std::min(1.0, t / shot.flight) : 1.0;
  out[0] = float(shot.from[0] + (shot.target[0] - shot.from[0]) * share);
  out[1] = float(shot.from[1] + (shot.target[1] - shot.from[1]) * share);
  out[2] = float(shot.from[2] + stone.rise * t - 0.5 * kGravity * t * t);
  if (share >= 1.0) {
    out[2] = shot.target[2];
  }
}

void land(Stone& stone, const float position[3]) {
  if (stone.shot.report) {
    queue_impact(stone.shot, position);
    if (log_line) {
      log_line("SIEGE shot %u came down at %.1f %.1f %.1f", stone.shot.shot, position[0],
               position[1], position[2]);
    }
  }
  entity::remove(stone_name(stone.shot.shot));
}

void fly_stones(double now) {
  for (auto it = stones.begin(); it != stones.end();) {
    Stone& stone = it->second;
    const double t = now - stone.start;
    float at[3];
    stone_at(stone, t, at);
    float distance = 0;
    if (physics::first_hit(stone.last, at, distance)) {
      const float dx = at[0] - stone.last[0];
      const float dy = at[1] - stone.last[1];
      const float dz = at[2] - stone.last[2];
      const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
      const float share = length > 1e-4f ? std::min(1.0f, distance / length) : 0.0f;
      const float hit[3] = {stone.last[0] + dx * share, stone.last[1] + dy * share,
                            stone.last[2] + dz * share};
      land(stone, hit);
      it = stones.erase(it);
      continue;
    }
    if (t >= stone.shot.flight + kFlightSlackSeconds) {
      const float target[3] = {stone.shot.target[0], stone.shot.target[1], stone.shot.target[2]};
      land(stone, target);
      it = stones.erase(it);
      continue;
    }
    if (void* body = entity::find(stone_name(stone.shot.shot))) {
      const float upright[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      entity::place_rotated(body, at, upright);
    }
    for (std::size_t i = 0; i < 3; ++i) {
      stone.last[i] = at[i];
    }
    ++it;
  }
}

void withdraw() {
  if (offered != Offer::None) {
    action_hints::clear_engine_hint(kSiegeHint);
    offered = Offer::None;
    offered_engine = 0;
  }
}

void offer(std::uint32_t id, Offer what) {
  if (offered == what && offered_engine == id) {
    return;
  }
  action_hints::EngineHint hint;
  hint.id = kSiegeHint;
  hint.key = kUseKey;
  SiegeAction action = SiegeAction::Operate;
  switch (what) {
    case Offer::Operate:
      hint.text = "Operate";
      action = SiegeAction::Operate;
      break;
    case Offer::Load:
      hint.text = "Load";
      action = SiegeAction::Load;
      break;
    case Offer::Fire:
      hint.text = "Fire";
      action = SiegeAction::Fire;
      break;
    case Offer::None:
      withdraw();
      return;
  }
  hint.on_fire = [id, action] { queue_use(id, action); };
  action_hints::set_engine_hint(hint);
  offered = what;
  offered_engine = id;
}

float distance_to(const Engine& engine, const float player[3]) {
  const float dx = engine.position[0] - player[0];
  const float dy = engine.position[1] - player[1];
  const float dz = engine.position[2] - player[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The engine the player works (the hint loads or fires it), else the nearest one they may take.
void offer_use(const float* player, bool allowed) {
  if (!player) {
    withdraw();
    return;
  }
  for (auto& [id, engine] : engines) {
    if (!engine.state.mine) {
      continue;
    }
    if (distance_to(engine, player) > kLeaveDistance) {
      if (!engine.leaving) {
        engine.leaving = true;
        queue_use(id, SiegeAction::Leave);
      }
      continue;
    }
    if (!allowed || engine.state.wrecked) {
      withdraw();
      return;
    }
    if (engine.state.cycle == SiegeCycle::Idle) {
      offer(id, Offer::Load);
    } else if (engine.state.cycle == SiegeCycle::Ready) {
      offer(id, Offer::Fire);
    } else {
      withdraw();
    }
    return;
  }
  std::uint32_t nearest = 0;
  float best = kUseDistance;
  for (const auto& [id, engine] : engines) {
    const SiegeEngineState& s = engine.state;
    if (!s.usable || s.wrecked || s.operated) {
      continue;
    }
    const float distance = distance_to(engine, player);
    if (distance <= best) {
      nearest = id;
      best = distance;
    }
  }
  if (!allowed || nearest == 0) {
    withdraw();
    return;
  }
  offer(nearest, Offer::Operate);
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
}

void apply(const SiegeEngineOrder& order) {
  switch (order.kind) {
    case SiegeOrderKind::Spawn:
      spawn(order);
      break;
    case SiegeOrderKind::Update:
      update(order);
      break;
    case SiegeOrderKind::Destroy:
      destroy(order.id);
      break;
  }
}

void fire(const SiegeShot& shot, double now) {
  if (stones.size() >= kMaxStones || stones.count(shot.shot)) {
    return;
  }
  Stone stone;
  stone.shot = shot;
  stone.start = now;
  stone.rise = std::sqrt(2.0f * kGravity * shot.apex);
  for (std::size_t i = 0; i < 3; ++i) {
    stone.last[i] = shot.from[i];
  }
  const auto engine = engines.find(shot.engine);
  const bool cannon = engine != engines.end() && engine->second.kind == SiegeEngineKind::Cannon;
  const float from[3] = {shot.from[0], shot.from[1], shot.from[2]};
  if (!entity::spawn_mesh(stone_name(shot.shot), from, cannon ? kCannonBall : kTrebuchetStone,
                          std::string()) &&
      log_line) {
    log_line("SIEGE shot %u: no stone to draw", shot.shot);
  }
  stones[shot.shot] = stone;
}

void frame(double now, const float* player, const float* view, bool allowed) {
  fly_stones(now);
  camera_forward_valid = false;
  if (view) {
    const float forward[3] = {view[1], view[5], view[9]};
    const float length =
        std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (length > 1e-4f) {
      for (std::size_t i = 0; i < 3; ++i) {
        camera_forward[i] = forward[i] / length;
      }
      camera_forward_valid = true;
    }
  }
  offer_use(player, allowed);
}

std::vector<integration::SiegeUse> take_uses() {
  std::lock_guard<std::mutex> lock(reports_mutex);
  return std::exchange(uses, {});
}

std::vector<integration::SiegeImpact> take_impacts() {
  std::lock_guard<std::mutex> lock(reports_mutex);
  return std::exchange(impacts, {});
}

void reset() {
  withdraw();
  for (const auto& [shot, stone] : stones) {
    entity::remove(stone_name(shot));
  }
  stones.clear();
  for (const auto& [id, engine] : engines) {
    entity::remove(engine_name(id));
  }
  engines.clear();
}

}  // namespace kcds::runtime::siege
