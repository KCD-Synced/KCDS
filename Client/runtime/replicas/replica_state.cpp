// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "replica_state.hpp"

#include "../attach/player_attach.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "held_items.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>

namespace kcds::game_client {

// Remote players shown smoothly: each one's owner as it was a moment ago on the
// owner's own clock, and the state shown this frame.
std::unordered_map<core::EntityId, kcd2s::integration::PlayerReplicaTimeline<RemoteState>>
    replica_timelines;
std::unordered_map<core::EntityId, RemoteState> replica_shown;
// Test hook: every 5 s, how many bodies moved more in one frame than anything walks. A
// body's steps between its own frames only: one not shown for a while is not counted.
void note_shown_step(core::EntityId id, const core::Vec3& at, double now) {
  static const bool wanted =
      GetEnvironmentVariableW(L"KCDS_TEST_CHARACTER_METRICS", nullptr, 0) > 0;
  if (!wanted) {
    return;
  }
  // The real players' bodies as this game shows them, for the desync check (desync487.py).
  constexpr std::uint32_t kFewestSyntheticId = 3;
  if (std::uint32_t(id) < kFewestSyntheticId) {
    LARGE_INTEGER clock;
    LARGE_INTEGER rate;
    QueryPerformanceCounter(&clock);
    QueryPerformanceFrequency(&rate);
    log("POSE shown %u %.3f %.3f %.3f %.3f", unsigned(id), double(clock.QuadPart) / rate.QuadPart,
        at.x, at.y, at.z);
  }
  constexpr double kLongestFrame = 0.05;
  constexpr float kFastestStep = 12.0f;
  constexpr float kVisibleStep = 0.15f;
  constexpr double kReportSeconds = 5.0;
  struct Last {
    core::Vec3 at;
    double when = -1.0;
  };
  static std::unordered_map<core::EntityId, Last> last;
  static unsigned steps = 0;
  static unsigned jumps = 0;
  static float largest = 0.0f;
  static double reported = now;
  Last& body = last[id];
  const double passed = now - body.when;
  if (body.when >= 0.0 && passed > 0.0 && passed < kLongestFrame) {
    const float step = core::distance(body.at, at);
    ++steps;
    if (step > kVisibleStep && step > float(passed) * kFastestStep) {
      ++jumps;
      largest = (std::max)(largest, step);
      log("JUMP body=%u %.2f m from (%.2f %.2f %.2f) to (%.2f %.2f %.2f) at %.3f", unsigned(id),
          step, body.at.x, body.at.y, body.at.z, at.x, at.y, at.z, now);
    }
  }
  body.at = at;
  body.when = now;
  if (now - reported >= kReportSeconds) {
    log("JUMPCHECK bodies=%zu steps=%u jumps=%u largest=%.2f", last.size(), steps, jumps, largest);
    steps = 0;
    jumps = 0;
    largest = 0.0f;
    reported = now;
  }
}

// The state of remote player `id` to show at `now`: its owner as it was a moment ago
// on the owner's clock, its position and rotation between the samples around that moment. An
// owner that sends no clock (an older client) is shown as it arrives.
const RemoteState& shown_remote_state(core::EntityId id, const RemoteState& latest, double now) {
  auto timeline = replica_timelines.try_emplace(id, kReplicaTeleportMeters).first;
  const RemoteState* selected = &timeline->second.shown(latest, now, replica_shown[id]);
  // A body attached to another (Player.attachTo) is shown from where this game shows
  // the target, so the network's delay does not open a gap between them.
  if (const auto* order = kcds::runtime::attach::order_of(std::uint32_t(id))) {
    kcd2s::integration::TimelinePose pose;
    kcds::runtime::attach::BodyPose target;
    bool known = false;
    if (order->flags & kcd2s::integration::kAttachToLocalPlayer) {
      known = kcds::runtime::attach::local_pose(target);
    } else if (const auto found = replica_shown.find(core::EntityId(order->target));
               found != replica_shown.end() && found->first != id) {
      const auto at = std::get<core::Vec3>(found->second.fields[local::Position]);
      const auto q = std::get<core::Quat>(found->second.fields[local::Rotation]);
      target.x = at.x;
      target.y = at.y;
      target.z = at.z;
      target.heading =
          std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
      known = true;
    }
    if (known) {
      RemoteState& placed = replica_shown[id];
      if (&*selected != &placed) {
        placed = *selected;
      }
      selected = &placed;
      const auto place = kcds::runtime::attach::beside(*order, target);
      pose.position.x = place.x;
      pose.position.y = place.y;
      pose.position.z = place.z;
      pose.rotation.w = std::cos(place.heading * 0.5f);
      pose.rotation.x = 0.0f;
      pose.rotation.y = 0.0f;
      pose.rotation.z = std::sin(place.heading * 0.5f);
      placed.fields[local::Position] = pose.position;
      placed.fields[local::Rotation] = pose.rotation;
    }
  }
  const RemoteState& shown = *selected;
  note_shown_step(id, std::get<core::Vec3>(shown.fields[local::Position]), now);
  // Dev trace for crowd tests (replicas/held_items.hpp): the focus body's timeline this frame.
  if (std::uint32_t(id) == crowd_trace_focus()) {
    const auto& line = timeline->second.timeline();
    log("CROWDT id=%u now=%.4f delay=%.4f offset=%.4f gap=%.4f late=%.4f samples=%zu", unsigned(id),
        now, line.delay(), line.offset(), line.gap(), line.lateness(), line.sample_count());
  }
  // Test hook: every frame's shown and newest position of each replica, to measure smoothness.
  static const bool trace = GetEnvironmentVariableW(L"KCDS_TEST_REPLICA_TRACE", nullptr, 0) > 0;
  if (trace) {
    kcd2s::integration::TimelinePose arrived, pose;
    arrived.position = std::get<core::Vec3>(latest.fields[local::Position]);
    arrived.rotation = std::get<core::Quat>(latest.fields[local::Rotation]);
    pose.position = std::get<core::Vec3>(shown.fields[local::Position]);
    pose.rotation = std::get<core::Quat>(shown.fields[local::Rotation]);
    const auto yaw = [](const core::Quat& q) {
      return std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    };
    log("SMOOTH player=%u now=%.4f shown=%.3f,%.3f,%.3f newest=%.3f,%.3f,%.3f yaw=%.3f "
        "newest_yaw=%.3f",
        id, now, pose.position.x, pose.position.y, pose.position.z, arrived.position.x,
        arrived.position.y, arrived.position.z, yaw(pose.rotation), yaw(arrived.rotation));
    if (latest.fields.size() > local::LookValid) {
      const auto target = std::get<core::Vec3>(latest.fields[local::LookTarget]);
      log("SMOOTH look player=%u valid=%u enabled=%u yaw=%.3f", id,
          unsigned(std::get<bool>(latest.fields[local::LookValid])),
          unsigned(std::get<bool>(latest.fields[local::LookEnabled])),
          std::atan2(-(target.x - arrived.position.x), target.y - arrived.position.y));
    }
  }
  return shown;
}
// A body's clips (the Animations field) as last decoded: the field changes far less often than a
// frame comes, and a crowd decodes it for every body several times a frame.
namespace {
struct DecodedAnimation {
  std::string source;
  std::optional<local::AnimationState> clips;
  bool valid = false;
};
std::unordered_map<std::uint64_t, DecodedAnimation> decoded_animations;
// How far a body's world matrix may be from what it is told and still count as there.
constexpr float kInPlaceTolerance = 0.0001f;
}  // namespace

const std::optional<local::AnimationState>& decoded_animation(core::EntityId id,
                                                              const std::string& field,
                                                              bool weapon) {
  DecodedAnimation& decoded = decoded_animations[(std::uint64_t(id) << 1) | (weapon ? 1 : 0)];
  if (!decoded.valid || decoded.source != field) {
    decoded.source = field;
    decoded.clips = local::decode_animation(field);
    decoded.valid = true;
  }
  return decoded.clips;
}

void forget_decoded_animation(core::EntityId id) {
  decoded_animations.erase(std::uint64_t(id) << 1);
  decoded_animations.erase((std::uint64_t(id) << 1) | 1);
}

void set_transform(void* entity, const core::Vec3& p, const core::Quat& q, unsigned reason) {
  float m[12];
  m[0] = 1 - 2 * (q.y * q.y + q.z * q.z);
  m[1] = 2 * (q.x * q.y - q.w * q.z);
  m[2] = 2 * (q.x * q.z + q.w * q.y);
  m[3] = p.x;
  m[4] = 2 * (q.x * q.y + q.w * q.z);
  m[5] = 1 - 2 * (q.x * q.x + q.z * q.z);
  m[6] = 2 * (q.y * q.z - q.w * q.x);
  m[7] = p.y;
  m[8] = 2 * (q.x * q.z - q.w * q.y);
  m[9] = 2 * (q.y * q.z + q.w * q.x);
  m[10] = 1 - 2 * (q.x * q.x + q.y * q.y);
  m[11] = p.z;
  // Moving an entity runs the engine's whole chain (physics, attachments, areas): one already
  // there, as most of a crowd is on most frames, stays.
  if (reason == 0) {
    const float* current = slot<const float*(__fastcall*)(void*)>(entity, 32)(entity);
    bool in_place = current != nullptr;
    for (int i = 0; in_place && i < 12; ++i) {
      in_place = std::fabs(current[i] - m[i]) <= kInPlaceTolerance;
    }
    if (in_place) {
      return;
    }
  }
  slot<void(__fastcall*)(void*, const float*, unsigned)>(entity, 30)(entity, m, reason);
}
core::Quat rotation(const float* m) {
  core::Quat q;
  float t = m[0] + m[5] + m[10];
  if (t > 0) {
    float s = std::sqrt(t + 1) * 2;
    q.w = s / 4;
    q.x = (m[9] - m[6]) / s;
    q.y = (m[2] - m[8]) / s;
    q.z = (m[4] - m[1]) / s;
  } else if (m[0] > m[5] && m[0] > m[10]) {
    float s = std::sqrt(1 + m[0] - m[5] - m[10]) * 2;
    q.w = (m[9] - m[6]) / s;
    q.x = s / 4;
    q.y = (m[1] + m[4]) / s;
    q.z = (m[2] + m[8]) / s;
  } else if (m[5] > m[10]) {
    float s = std::sqrt(1 + m[5] - m[0] - m[10]) * 2;
    q.w = (m[2] - m[8]) / s;
    q.x = (m[1] + m[4]) / s;
    q.y = s / 4;
    q.z = (m[6] + m[9]) / s;
  } else {
    float s = std::sqrt(1 + m[10] - m[0] - m[5]) * 2;
    q.w = (m[4] - m[1]) / s;
    q.x = (m[2] + m[8]) / s;
    q.y = (m[6] + m[9]) / s;
    q.z = s / 4;
  }
  return q;
}

}  // namespace kcds::game_client
