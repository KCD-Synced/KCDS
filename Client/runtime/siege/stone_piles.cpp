#include "stone_piles.hpp"

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
#include "siege_math.hpp"

namespace kcds::runtime::siege::stones {
namespace {

namespace integration = kcd2s::integration;
using integration::StonePileMessageKind;
using integration::StonePileOrder;
using integration::StonePileReport;

namespace entity = props::entity;

constexpr const char* kStoneA =
    "objects/manmade/task_specific_props/combat/castle_siege/stone_hurling/hurling_stone.cgf";
constexpr const char* kStoneB =
    "objects/manmade/task_specific_props/combat/castle_siege/stone_hurling/hurling_stone_b.cgf";

// A stone of the pile: its mesh, where it lies from the pile's origin and how it is turned
// (x, y, z, w).
struct PileStone {
  const char* mesh;
  float offset[3];
  float rotation[4];
};

// The stones of the game's prefab hurlingStone/hurlingStonePile_a (its Rotate is w, x, y, z),
// lowest first: a pile that runs low loses its top stones first.
constexpr PileStone kPileStones[] = {
    {kStoneB, {-0.0385f, 0.1165f, -0.0068f}, {0.0f, 0.0f, -0.4540f, 0.8910f}},
    {kStoneB, {0.3544f, 0.2599f, 0.0f}, {0.0f, 0.0f, 0.7880f, 0.6157f}},
    {kStoneA, {-0.3828f, 0.7813f, 0.1020f}, {0.5620f, -0.4391f, 0.5523f, 0.4315f}},
    {kStoneA, {0.1983f, 0.5762f, 0.1257f}, {0.0958f, 0.7432f, -0.0507f, 0.6602f}},
    {kStoneA, {0.0891f, 0.1310f, 0.1389f}, {-0.1066f, 0.3590f, -0.7452f, 0.5518f}},
    {kStoneA, {0.0603f, 0.7877f, 0.1747f}, {-0.1784f, 0.7154f, -0.6123f, -0.2855f}},
    {kStoneA, {0.3897f, 0.4754f, 0.2153f}, {0.0987f, 0.1998f, -0.7739f, 0.5927f}},
    {kStoneB, {-0.0552f, 0.5632f, 0.2272f}, {0.2001f, 0.0624f, -0.7969f, 0.5666f}},
    {kStoneA, {-0.1806f, 0.3627f, 0.2420f}, {-0.1478f, 0.9890f, -0.0013f, -0.0086f}},
};
constexpr std::size_t kPileStoneCount = sizeof(kPileStones) / sizeof(kPileStones[0]);

// The server lets a player this close take a stone (siege/stones.cjs TAKE_REACH, 6 m); the hint
// asks nearer, at arm's length of the pile.
constexpr float kTakeDistance = 2.2f;
// The heave: from this high over the player's feet and this far ahead of their head, at this
// speed (metres per second) along where the camera looks.
constexpr float kThrowHeight = 1.6f;
constexpr float kThrowAhead = 0.5f;
constexpr float kThrowSpeed = 8.0f;
constexpr float kGravity = 9.81f;
// A stone the world did not stop by then is reported where it is.
constexpr double kMaxFlightSeconds = 6.0;
// One throw per this long, and this many stones in the air at once (the server keeps 8).
constexpr double kThrowEverySeconds = 1.5;
constexpr std::size_t kMaxInFlight = 4;
// The game's "use" control (action_hints' key).
constexpr const char* kUseKey = "primary_action";
constexpr std::int64_t kPileHint = -63;
constexpr std::size_t kMaxReports = 32;

struct Pile {
  float position[3]{};
  float rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
  std::int64_t stones = -1;
  bool usable = false;
  // How many of its stones are drawn.
  std::size_t shown = 0;
};

struct Thrown {
  std::uint32_t pile = 0;
  double start = 0;
  float from[3]{};
  float velocity[3]{};
  float last[3]{};
};

Logger log_line = nullptr;
// Engine thread only: server pile id -> what it shows.
std::unordered_map<std::uint32_t, Pile> piles;
// Engine thread only: the player's stones in the air, by their own number.
std::unordered_map<std::uint32_t, Thrown> thrown;
std::uint32_t next_thrown = 1;
double last_throw = -1e9;
std::uint32_t offered_pile = 0;
// Where the camera looked at the last frame: the stone goes there.
float camera_forward[3]{};
bool camera_forward_valid = false;
// The local player's feet at the last frame.
float player_position[3]{};
bool player_valid = false;
double last_frame = 0;

std::mutex reports_mutex;
std::vector<StonePileReport> reports;

std::string stone_name(std::uint32_t pile, std::size_t index) {
  return "kcds_pile_" + std::to_string(pile) + "_" + std::to_string(index);
}

std::string thrown_name(std::uint32_t number) {
  return "kcds_thrown_stone_" + std::to_string(number);
}

void queue_report(const StonePileReport& report) {
  std::lock_guard<std::mutex> lock(reports_mutex);
  if (reports.size() < kMaxReports) {
    reports.push_back(report);
  }
}

std::size_t wanted(const Pile& pile) {
  if (pile.stones < 0) {
    return kPileStoneCount;
  }
  return std::min<std::size_t>(std::size_t(pile.stones), kPileStoneCount);
}

// Draws as many of the pile's stones as it has, removing or adding the top ones.
void lay(std::uint32_t id, Pile& pile) {
  const std::size_t count = wanted(pile);
  while (pile.shown > count) {
    --pile.shown;
    entity::remove(stone_name(id, pile.shown));
  }
  while (pile.shown < count) {
    const PileStone& stone = kPileStones[pile.shown];
    float offset[3];
    math::rotate(pile.rotation, stone.offset, offset);
    const float at[3] = {pile.position[0] + offset[0], pile.position[1] + offset[1],
                         pile.position[2] + offset[2]};
    void* body = entity::spawn_mesh(stone_name(id, pile.shown), at, stone.mesh, std::string());
    if (!body) {
      if (log_line) {
        log_line("SIEGE pile %u: no stone mesh to draw", id);
      }
      return;
    }
    float rotation[4];
    math::multiply(pile.rotation, stone.rotation, rotation);
    entity::rotate(body, rotation);
    ++pile.shown;
  }
}

void clear(std::uint32_t id, Pile& pile) {
  while (pile.shown > 0) {
    --pile.shown;
    entity::remove(stone_name(id, pile.shown));
  }
}

void withdraw() {
  if (offered_pile != 0) {
    action_hints::clear_engine_hint(kPileHint);
    offered_pile = 0;
  }
}

// The player heaves a stone off the pile: the take goes to the server, the stone flies here.
void heave(std::uint32_t id) {
  const auto found = piles.find(id);
  if (found == piles.end() || !player_valid || !camera_forward_valid ||
      thrown.size() >= kMaxInFlight || last_frame - last_throw < kThrowEverySeconds) {
    return;
  }
  last_throw = last_frame;
  StonePileReport take;
  take.kind = StonePileMessageKind::Take;
  take.pile = id;
  queue_report(take);
  Thrown stone;
  stone.pile = id;
  stone.start = last_frame;
  for (std::size_t i = 0; i < 3; ++i) {
    stone.from[i] = player_position[i] + camera_forward[i] * kThrowAhead;
    stone.velocity[i] = camera_forward[i] * kThrowSpeed;
  }
  stone.from[2] += kThrowHeight;
  for (std::size_t i = 0; i < 3; ++i) {
    stone.last[i] = stone.from[i];
  }
  const std::uint32_t number = next_thrown++;
  if (!entity::spawn_mesh(thrown_name(number), stone.from, kStoneA, std::string()) && log_line) {
    log_line("SIEGE pile %u: no stone to throw", id);
  }
  thrown[number] = stone;
  if (log_line) {
    log_line("SIEGE pile %u: the player throws stone %u", id, number);
  }
}

void offer(std::uint32_t id) {
  if (offered_pile == id) {
    return;
  }
  if (id == 0) {
    withdraw();
    return;
  }
  action_hints::EngineHint hint;
  hint.id = kPileHint;
  hint.key = kUseKey;
  hint.text = "Throw a stone";
  hint.on_fire = [id] { heave(id); };
  action_hints::set_engine_hint(hint);
  offered_pile = id;
}

// The nearest usable pile with stones the player stands at.
void offer_throw(bool allowed) {
  if (!player_valid || !allowed) {
    withdraw();
    return;
  }
  std::uint32_t nearest = 0;
  float best = kTakeDistance;
  for (const auto& [id, pile] : piles) {
    if (!pile.usable || pile.stones == 0) {
      continue;
    }
    const float distance = math::distance(player_position, pile.position);
    if (distance <= best) {
      best = distance;
      nearest = id;
    }
  }
  offer(nearest);
}

void land(std::uint32_t number, const Thrown& stone, const float position[3]) {
  StonePileReport impact;
  impact.kind = StonePileMessageKind::Impact;
  impact.pile = stone.pile;
  for (std::size_t i = 0; i < 3; ++i) {
    impact.position[i] = position[i];
  }
  queue_report(impact);
  entity::remove(thrown_name(number));
  if (log_line) {
    log_line("SIEGE stone %u came down at %.1f %.1f %.1f", number, position[0], position[1],
             position[2]);
  }
}

void fly(double now) {
  for (auto it = thrown.begin(); it != thrown.end();) {
    Thrown& stone = it->second;
    const float t = float(now - stone.start);
    const float at[3] = {stone.from[0] + stone.velocity[0] * t,
                         stone.from[1] + stone.velocity[1] * t,
                         stone.from[2] + stone.velocity[2] * t - 0.5f * kGravity * t * t};
    float distance = 0;
    if (physics::first_hit(stone.last, at, distance)) {
      float step[3];
      for (std::size_t i = 0; i < 3; ++i) {
        step[i] = at[i] - stone.last[i];
      }
      const float length = std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]);
      const float share = length > 1e-4f ? std::min(1.0f, distance / length) : 0.0f;
      const float hit[3] = {stone.last[0] + step[0] * share, stone.last[1] + step[1] * share,
                            stone.last[2] + step[2] * share};
      land(it->first, stone, hit);
      it = thrown.erase(it);
      continue;
    }
    if (t >= kMaxFlightSeconds) {
      land(it->first, stone, at);
      it = thrown.erase(it);
      continue;
    }
    if (void* body = entity::find(thrown_name(it->first))) {
      const float upright[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      entity::place_rotated(body, at, upright);
    }
    for (std::size_t i = 0; i < 3; ++i) {
      stone.last[i] = at[i];
    }
    ++it;
  }
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
}

void apply(const StonePileOrder& order) {
  switch (order.kind) {
    case StonePileMessageKind::Spawn: {
      auto found = piles.find(order.id);
      if (found != piles.end()) {
        clear(order.id, found->second);
      }
      Pile pile;
      for (std::size_t i = 0; i < 3; ++i) {
        pile.position[i] = order.position[i];
      }
      for (std::size_t i = 0; i < 4; ++i) {
        pile.rotation[i] = order.rotation[i];
      }
      pile.stones = order.stones;
      pile.usable = order.usable;
      lay(order.id, pile);
      piles[order.id] = pile;
      if (log_line) {
        log_line("SIEGE pile %u: %lld stones%s", order.id, static_cast<long long>(order.stones),
                 order.usable ? "" : ", not usable");
      }
      break;
    }
    case StonePileMessageKind::Update: {
      const auto found = piles.find(order.id);
      if (found == piles.end()) {
        return;
      }
      found->second.stones = order.stones;
      found->second.usable = order.usable;
      lay(order.id, found->second);
      break;
    }
    case StonePileMessageKind::Destroy: {
      const auto found = piles.find(order.id);
      if (found == piles.end()) {
        return;
      }
      clear(order.id, found->second);
      piles.erase(found);
      if (offered_pile == order.id) {
        withdraw();
      }
      break;
    }
    default:
      break;
  }
}

void frame(double now, const float* player, const float* view, bool allowed) {
  last_frame = now;
  player_valid = player != nullptr;
  if (player) {
    for (std::size_t i = 0; i < 3; ++i) {
      player_position[i] = player[i];
    }
  }
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
  fly(now);
  offer_throw(allowed);
}

std::vector<StonePileReport> take_reports() {
  std::lock_guard<std::mutex> lock(reports_mutex);
  return std::exchange(reports, {});
}

void reset() {
  withdraw();
  for (const auto& [number, stone] : thrown) {
    entity::remove(thrown_name(number));
  }
  thrown.clear();
  for (auto& [id, pile] : piles) {
    clear(id, pile);
  }
  piles.clear();
}

}  // namespace kcds::runtime::siege::stones
