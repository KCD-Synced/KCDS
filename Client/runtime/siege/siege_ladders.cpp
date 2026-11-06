#include "siege_ladders.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "../action_hints/resource_action_hints.hpp"
#include "../props/prop_entity.hpp"
#include "siege_math.hpp"

namespace kcds::runtime::siege::ladders {
namespace {

namespace integration = kcd2s::integration;
using integration::LadderAction;
using integration::LadderKind;
using integration::LadderMessageKind;
using integration::LadderPose;
using integration::SiegeLadderOrder;
using integration::SiegeLadderReport;

namespace entity = props::entity;

// The game's climbable ladder (Entities/Ladder.ent).
constexpr const char* kLadderClass = "Ladder";
// The meshes of siege/ladders.cjs KINDS, in LadderKind order; each has a broken variant.
constexpr const char* kMeshFolder = "objects/manmade/common_fixtures/ladders/";
constexpr const char* kMeshStems[] = {"ladder_siege_10m", "ladder_rustic_575", "ladder_rustic_350",
                                      "ladder_rustic_225"};
// A ladder on the ground lies a quarter turn from upright, its top away from the wall.
constexpr float kLyingDegrees = -90.0f;
// The pushed ladder: the defender's shove, then the fall (seconds). Their sum is under the
// server's PUSH_MS (4.5 s) and over its PUSH_MS - EARLY_MS (3 s), so the actor's report of the end
// is heard.
constexpr double kShoveSeconds = 2.0;
constexpr double kFallSeconds = 1.5;
// The lift of a raise (seconds), under RAISE_MS (5.7 s), over RAISE_MS - EARLY_MS (4.2 s).
constexpr double kLiftSeconds = 4.6;
// How near the top of a standing ladder (or a lying ladder's length) the hint asks the player to
// be (metres); the server allows the height plus 6 m from the foot.
constexpr float kReach = 2.5f;
// The game's "use" control (action_hints' key).
constexpr const char* kUseKey = "primary_action";
constexpr std::int64_t kLadderHint = -62;
constexpr std::size_t kMaxReports = 32;

struct Ladder {
  LadderKind kind = LadderKind::Siege;
  float height = 0.0f;
  float lean = 0.0f;
  float position[3]{};
  float rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
  LadderPose pose = LadderPose::Standing;
  bool usable = false;
  bool mine = false;
  // When the pose was entered here, for the fall and the lift.
  double since = 0;
  // The actor's game has said the step is over.
  bool settled = false;
  // The climbable entity stands (else the prop shows the pose).
  bool climbable = false;
};

enum class Offer { None, Push, Raise };

Logger log_line = nullptr;
// Engine thread only: server ladder id -> what it shows.
std::unordered_map<std::uint32_t, Ladder> ladders;
std::uint32_t offered_ladder = 0;
Offer offered = Offer::None;
// An ask sent and not answered yet: no other is offered meanwhile, for at most kAnswerSeconds.
std::uint32_t asked_ladder = 0;
double asked_at = 0;
constexpr double kAnswerSeconds = 3.0;
// The clock of the last frame, for the ask's time.
double last_frame = 0;

std::mutex reports_mutex;
std::vector<SiegeLadderReport> reports;

std::string climbable_name(std::uint32_t id) {
  return "kcds_ladder_" + std::to_string(id);
}

std::string prop_name(std::uint32_t id) {
  return "kcds_ladder_prop_" + std::to_string(id);
}

std::string mesh_of(LadderKind kind, bool broken) {
  std::string mesh = std::string(kMeshFolder) + kMeshStems[std::size_t(kind)];
  if (broken) {
    mesh += "_broken";
  }
  return mesh + ".cgf";
}

void queue_report(const SiegeLadderReport& report) {
  std::lock_guard<std::mutex> lock(reports_mutex);
  if (reports.size() < kMaxReports) {
    reports.push_back(report);
  }
}

// The ladder's rotation when it stands `degrees` from upright about its own x axis (positive:
// its top towards -y, the wall).
void tilted(const Ladder& ladder, float degrees, float out[4]) {
  float tilt[4];
  math::axis_x(degrees * math::kRadiansPerDegree, tilt);
  math::multiply(ladder.rotation, tilt, out);
}

// Where the top of a ladder is at that tilt.
void top_at(const Ladder& ladder, float degrees, float out[3]) {
  float rotation[4];
  tilted(ladder, degrees, rotation);
  const float up[3] = {0.0f, 0.0f, ladder.height};
  float along[3];
  math::rotate(rotation, up, along);
  for (std::size_t i = 0; i < 3; ++i) {
    out[i] = ladder.position[i] + along[i];
  }
}

void remove_climbable(std::uint32_t id, Ladder& ladder) {
  if (ladder.climbable) {
    entity::remove(climbable_name(id));
    ladder.climbable = false;
  }
}

void remove_prop(std::uint32_t id) {
  entity::remove(prop_name(id));
}

// The game's own ladder at its lean; false when the game would not make it.
bool build_climbable(std::uint32_t id, Ladder& ladder) {
  remove_prop(id);
  entity::remove(climbable_name(id));
  float rotation[4];
  tilted(ladder, ladder.lean, rotation);
  const std::string model = mesh_of(ladder.kind, false);
  const std::vector<entity::ScriptProperty> properties = {
      {"fileModel", model.c_str(), 0.0f},
      {"height", nullptr, ladder.height},
  };
  ladder.climbable = entity::spawn_class_with(climbable_name(id), kLadderClass, ladder.position,
                                              rotation, properties) != nullptr;
  return ladder.climbable;
}

// The prop that shows a ladder that is not standing, at the tilt.
void build_prop(std::uint32_t id, Ladder& ladder, float degrees) {
  remove_climbable(id, ladder);
  remove_prop(id);
  const bool broken = ladder.pose == LadderPose::Broken;
  void* body = entity::spawn_mesh(prop_name(id), ladder.position, mesh_of(ladder.kind, broken),
                                  std::string());
  if (!body && broken) {
    body = entity::spawn_mesh(prop_name(id), ladder.position, mesh_of(ladder.kind, false),
                              std::string());
  }
  if (!body) {
    if (log_line) {
      log_line("SIEGE ladder %u: no mesh to show", id);
    }
    return;
  }
  float rotation[4];
  tilted(ladder, degrees, rotation);
  entity::rotate(body, rotation);
}

// The tilt a ladder that is not standing has `t` seconds into its pose, and whether its step is
// over by then.
float tilt_at(const Ladder& ladder, double t, bool& over) {
  over = true;
  switch (ladder.pose) {
    case LadderPose::Pushed: {
      if (t < kShoveSeconds) {
        over = false;
        return ladder.lean;
      }
      const double share = std::min(1.0, (t - kShoveSeconds) / kFallSeconds);
      over = share >= 1.0;
      // It falls as a body falls: slowly at first, fastest when it meets the ground.
      return float(ladder.lean + (kLyingDegrees - ladder.lean) * share * share);
    }
    case LadderPose::Raising: {
      const double share = std::min(1.0, t / kLiftSeconds);
      over = share >= 1.0;
      const double eased = share * share * (3.0 - 2.0 * share);
      return float(kLyingDegrees + (ladder.lean - kLyingDegrees) * eased);
    }
    case LadderPose::Standing:
      return ladder.lean;
    default:
      return kLyingDegrees;
  }
}

// Shows a ladder as its pose says, from the start of that pose.
void show(std::uint32_t id, Ladder& ladder) {
  if (ladder.pose == LadderPose::Standing) {
    if (!build_climbable(id, ladder) && log_line) {
      log_line("SIEGE ladder %u: the game made no climbable ladder", id);
    }
    return;
  }
  bool over = false;
  build_prop(id, ladder, tilt_at(ladder, 0.0, over));
}

void withdraw() {
  if (offered != Offer::None) {
    action_hints::clear_engine_hint(kLadderHint);
    offered = Offer::None;
    offered_ladder = 0;
  }
}

void spawn(const SiegeLadderOrder& order, double now) {
  Ladder ladder;
  ladder.kind = order.ladder;
  ladder.height = order.height;
  ladder.lean = order.lean;
  for (std::size_t i = 0; i < 3; ++i) {
    ladder.position[i] = order.position[i];
  }
  for (std::size_t i = 0; i < 4; ++i) {
    ladder.rotation[i] = order.rotation[i];
  }
  ladder.pose = order.pose;
  ladder.usable = order.usable;
  ladder.mine = order.mine;
  ladder.since = now;
  const auto found = ladders.find(order.id);
  if (found != ladders.end()) {
    remove_climbable(order.id, found->second);
  }
  show(order.id, ladder);
  ladders[order.id] = ladder;
  if (log_line) {
    log_line("SIEGE ladder %u: kind %u, %.2f m, pose %u", order.id, unsigned(order.ladder),
             order.height, unsigned(order.pose));
  }
}

void pose(const SiegeLadderOrder& order, double now) {
  const auto found = ladders.find(order.id);
  if (found == ladders.end()) {
    return;
  }
  Ladder& ladder = found->second;
  const LadderPose before = ladder.pose;
  ladder.usable = order.usable;
  ladder.mine = order.mine;
  if (asked_ladder == order.id) {
    asked_ladder = 0;
  }
  if (before == order.pose) {
    return;
  }
  ladder.pose = order.pose;
  ladder.since = now;
  ladder.settled = false;
  if (log_line) {
    log_line("SIEGE ladder %u: pose %u -> %u%s", order.id, unsigned(before), unsigned(order.pose),
             order.mine ? " (this player's step)" : "");
  }
  // A ladder that came down by the server's word lies at once, as one that stands up again.
  if (ladder.pose == LadderPose::Down && before == LadderPose::Pushed) {
    build_prop(order.id, ladder, kLyingDegrees);
    return;
  }
  show(order.id, ladder);
}

void destroy(std::uint32_t id) {
  const auto found = ladders.find(id);
  if (found == ladders.end()) {
    return;
  }
  remove_climbable(id, found->second);
  remove_prop(id);
  ladders.erase(found);
  if (offered_ladder == id) {
    withdraw();
  }
  if (asked_ladder == id) {
    asked_ladder = 0;
  }
}

// Turns the ladders mid-step and, for the steps of this game's player, reports their end.
void animate(double now) {
  for (auto& [id, ladder] : ladders) {
    if (ladder.pose != LadderPose::Pushed && ladder.pose != LadderPose::Raising) {
      continue;
    }
    bool over = false;
    const float degrees = tilt_at(ladder, now - ladder.since, over);
    if (void* body = entity::find(prop_name(id))) {
      float rotation[4];
      tilted(ladder, degrees, rotation);
      entity::rotate(body, rotation);
    }
    if (over && ladder.mine && !ladder.settled) {
      ladder.settled = true;
      SiegeLadderReport report;
      report.kind = LadderMessageKind::Settled;
      report.ladder = id;
      report.pose = ladder.pose == LadderPose::Pushed ? LadderPose::Down : LadderPose::Standing;
      queue_report(report);
    }
  }
}

void offer(std::uint32_t id, Offer what) {
  if (offered == what && offered_ladder == id) {
    return;
  }
  if (what == Offer::None) {
    withdraw();
    return;
  }
  action_hints::EngineHint hint;
  hint.id = kLadderHint;
  hint.key = kUseKey;
  hint.text = what == Offer::Push ? "Push the ladder" : "Raise the ladder";
  const LadderAction action = what == Offer::Push ? LadderAction::Push : LadderAction::Raise;
  hint.on_fire = [id, action] {
    SiegeLadderReport report;
    report.kind = LadderMessageKind::Use;
    report.ladder = id;
    report.action = action;
    queue_report(report);
    asked_ladder = id;
    asked_at = last_frame;
    if (log_line) {
      log_line("SIEGE ladder %u: the player asks %s", id,
               action == LadderAction::Push ? "push" : "raise");
    }
  };
  action_hints::set_engine_hint(hint);
  offered = what;
  offered_ladder = id;
}

// The distance from the point to the segment from `a` to `b`.
float distance_to_segment(const float point[3], const float a[3], const float b[3]) {
  float ab[3];
  float ap[3];
  for (std::size_t i = 0; i < 3; ++i) {
    ab[i] = b[i] - a[i];
    ap[i] = point[i] - a[i];
  }
  const float length = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
  float share = length > 1e-6f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / length : 0.0f;
  share = std::clamp(share, 0.0f, 1.0f);
  float closest[3];
  for (std::size_t i = 0; i < 3; ++i) {
    closest[i] = a[i] + ab[i] * share;
  }
  return math::distance(point, closest);
}

// The standing ladder whose top the player is at (push), else the lying one beside them (raise).
void offer_use(double now, const float* player, bool allowed) {
  if (asked_ladder != 0 && now - asked_at > kAnswerSeconds) {
    asked_ladder = 0;
  }
  if (!player || !allowed || asked_ladder != 0) {
    withdraw();
    return;
  }
  std::uint32_t nearest = 0;
  Offer what = Offer::None;
  float best = kReach;
  for (const auto& [id, ladder] : ladders) {
    if (!ladder.usable) {
      continue;
    }
    float distance = kReach + 1.0f;
    if (ladder.pose == LadderPose::Standing) {
      float top[3];
      top_at(ladder, ladder.lean, top);
      distance = math::distance(player, top);
    } else if (ladder.pose == LadderPose::Down) {
      float top[3];
      top_at(ladder, kLyingDegrees, top);
      distance = distance_to_segment(player, ladder.position, top);
    } else {
      continue;
    }
    if (distance <= best) {
      best = distance;
      nearest = id;
      what = ladder.pose == LadderPose::Standing ? Offer::Push : Offer::Raise;
    }
  }
  offer(nearest, what);
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
}

void apply(const SiegeLadderOrder& order, double now) {
  switch (order.kind) {
    case LadderMessageKind::Spawn:
      spawn(order, now);
      break;
    case LadderMessageKind::Pose:
      pose(order, now);
      break;
    case LadderMessageKind::Destroy:
      destroy(order.id);
      break;
    case LadderMessageKind::Refused:
      // Nothing changed here for the ask; it may be offered again.
      if (log_line && asked_ladder != 0) {
        log_line("SIEGE ladder %u: the server refused the player's ask", asked_ladder);
      }
      asked_ladder = 0;
      break;
    default:
      break;
  }
}

void frame(double now, const float* player, bool allowed) {
  last_frame = now;
  animate(now);
  offer_use(now, player, allowed);
}

std::vector<SiegeLadderReport> take_reports() {
  std::lock_guard<std::mutex> lock(reports_mutex);
  return std::exchange(reports, {});
}

void reset() {
  withdraw();
  asked_ladder = 0;
  for (auto& [id, ladder] : ladders) {
    remove_climbable(id, ladder);
    remove_prop(id);
  }
  ladders.clear();
}

}  // namespace kcds::runtime::siege::ladders
