#include "placer.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "../physics/line_of_sight.hpp"
#include "ghosts.hpp"
#include "pose_math.hpp"

namespace kcds::runtime::props::placer {
namespace {

using kcd2s::integration::PlacerEvent;
using kcd2s::integration::PlacerKind;
using kcd2s::integration::PlacerOrder;
using kcd2s::integration::PlacerReport;

constexpr float kPi = 3.14159265f;
constexpr float kTurnDegreesPerSecond = 90.0f;
constexpr float kGroundSearch = 50.0f;
constexpr double kMoveInterval = 0.1;
constexpr float kMoveDistance = 0.05f;
constexpr float kMoveTurn = 0.002f;
// The actions a placing player keeps, as a cuffed one does: walking, looking around, the menu.
constexpr const char* kKeptPrefixes[] = {"move", "rotate", "walk", "menu", "pause"};
constexpr unsigned kReleased = 2;

struct Key {
  int virtual_key;
  bool down = false;
  bool armed = false;  // let go since the session began

  // True on the frame the key goes down.
  bool pressed(bool counts) {
    const bool now = counts && (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
    const bool edge = now && !down && armed;
    if (!now) {
      armed = true;
    }
    down = now;
    return edge;
  }
};

struct Spot {
  float position[3]{};
  float rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
  float normal[3]{0.0f, 0.0f, 1.0f};
  float distance = 0.0f;
  bool on_surface = false;
  bool valid = false;
};

struct Session {
  PlacerOrder order;
  ghosts::Preview preview;
  bool vetoed = false;
  float turn_degrees = 0.0f;  // what the player turned it
  double last_frame = -1;
  Spot spot;
  bool has_spot = false;
  // The last move report.
  double moved_at = -1;
  Spot reported;
  bool has_reported = false;
  Key confirm{VK_LBUTTON};
  Key cancel{VK_RBUTTON};
};

Logger log_line = nullptr;
bool running = false;
Session session;

float length3(const float v[3]) {
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

bool finite3(const float v[3]) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// Where the crosshair puts the preview this frame.
Spot aim(const float* camera) {
  Spot spot;
  const float origin[3] = {camera[3], camera[7], camera[11]};
  float forward[3] = {camera[1], camera[5], camera[9]};
  const float forward_length = length3(forward);
  if (!(forward_length > 1e-4f)) {
    return spot;
  }
  for (float& axis : forward) {
    axis /= forward_length;
  }
  const PlacerOrder& order = session.order;
  const float end[3] = {origin[0] + forward[0] * order.range, origin[1] + forward[1] * order.range,
                        origin[2] + forward[2] * order.range};
  float distance = 0.0f;
  if (physics::first_hit(origin, end, distance, spot.normal)) {
    for (int axis = 0; axis < 3; ++axis) {
      spot.position[axis] = origin[axis] + forward[axis] * distance;
    }
    spot.distance = distance;
    spot.on_surface = true;
  } else {
    const float along = std::fmin(order.fallback_distance, order.range);
    for (int axis = 0; axis < 3; ++axis) {
      spot.position[axis] = origin[axis] + forward[axis] * along;
    }
    spot.distance = along;
    const float below[3] = {spot.position[0], spot.position[1], spot.position[2] - kGroundSearch};
    float drop = 0.0f;
    if (physics::first_hit(spot.position, below, drop, spot.normal)) {
      spot.position[2] -= drop;
      spot.on_surface = true;
    }
  }
  spot.position[2] += order.vertical_offset;
  // The turn: the server's yaw and the player's, then the camera's heading, then the surface.
  float degrees = order.yaw + session.turn_degrees;
  float heading = 0.0f;
  if (order.flags & kcd2s::integration::kPlacerFaceAwayFromPlayer) {
    heading = std::atan2(-forward[0], forward[1]);
  }
  float yaw[4];
  pose_math::about_z(degrees * kPi / 180.0f + heading, yaw);
  if ((order.flags & kcd2s::integration::kPlacerAlignToSurface) && spot.on_surface) {
    float tilt[4];
    pose_math::up_onto(spot.normal, tilt);
    pose_math::multiply(tilt, yaw, spot.rotation);
  } else {
    std::memcpy(spot.rotation, yaw, sizeof(yaw));
  }
  pose_math::normalize(spot.rotation);
  return spot;
}

bool allowed(const Spot& spot, const float* player) {
  const PlacerOrder& order = session.order;
  if (session.vetoed || !finite3(spot.position)) {
    return false;
  }
  if ((order.flags & kcd2s::integration::kPlacerRequireSurface) && !spot.on_surface) {
    return false;
  }
  if (order.max_slope >= 0.0f && spot.on_surface) {
    const float cosine = std::fmax(-1.0f, std::fmin(1.0f, spot.normal[2]));
    if (std::acos(cosine) * 180.0f / kPi > order.max_slope) {
      return false;
    }
  }
  if (player) {
    const float offset[3] = {spot.position[0] - player[0], spot.position[1] - player[1],
                             spot.position[2] - player[2]};
    const float from_player = length3(offset);
    if (order.min_distance >= 0.0f && from_player < order.min_distance) {
      return false;
    }
    if (order.max_distance >= 0.0f && from_player > order.max_distance) {
      return false;
    }
  }
  return true;
}

PlacerReport report_of(PlacerEvent event, const Spot* spot) {
  PlacerReport report;
  report.event = event;
  report.session = session.order.session;
  if (spot) {
    report.valid = spot->valid;
    std::memcpy(report.position.data(), spot->position, sizeof(spot->position));
    std::memcpy(report.rotation.data(), spot->rotation, sizeof(spot->rotation));
    std::memcpy(report.normal.data(), spot->normal, sizeof(spot->normal));
    report.distance = spot->distance;
    report.on_surface = spot->on_surface;
  }
  return report;
}

void finish(const char* why) {
  ghosts::hide(session.preview);
  running = false;
  if (log_line) {
    log_line("PLACER %u ended: %s", session.order.session, why);
  }
}

void tint(bool valid) {
  const auto& rgba = valid ? session.order.valid_tint : session.order.invalid_tint;
  const bool changed = !session.preview.tint.present || session.preview.tint.rgba != rgba;
  session.preview.tint.present = true;
  session.preview.tint.rgba = rgba;
  if (changed) {
    ghosts::fade(session.preview);
  }
}

bool moved_enough(const Spot& spot) {
  if (!session.has_reported || spot.valid != session.reported.valid) {
    return true;
  }
  const float offset[3] = {spot.position[0] - session.reported.position[0],
                           spot.position[1] - session.reported.position[1],
                           spot.position[2] - session.reported.position[2]};
  float turn = 0.0f;
  for (int i = 0; i < 4; ++i) {
    turn += std::fabs(spot.rotation[i] - session.reported.rotation[i]);
  }
  return length3(offset) > kMoveDistance || turn > kMoveTurn;
}

void begin(const PlacerOrder& order) {
  if (running) {
    ghosts::hide(session.preview);
  }
  session = Session{};
  session.order = order;
  session.preview.name = "kcds_placer_" + std::to_string(order.session);
  session.preview.pieces = order.pieces;
  session.preview.scale = order.scale;
  session.preview.tint.present = true;
  session.preview.tint.rgba = order.invalid_tint;
  running = true;
  if (log_line) {
    log_line("PLACER %u begun: %s (%zu meshes), range %.1f", order.session,
             order.pieces.front().model.c_str(), order.pieces.size(), order.range);
  }
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
}

void apply(const PlacerOrder& order) {
  if (order.kind == PlacerKind::Begin) {
    begin(order);
    return;
  }
  if (!running || order.session != session.order.session) {
    return;
  }
  if (order.kind == PlacerKind::Veto) {
    session.vetoed = order.vetoed;
  } else if (order.kind == PlacerKind::End) {
    finish("by the server");
  }
}

void frame(const Frame& frame, std::vector<PlacerReport>& reports) {
  if (!running) {
    return;
  }
  const double step = session.last_frame < 0 ? 0.0 : frame.now - session.last_frame;
  session.last_frame = frame.now;
  // The turn keys.
  const bool left = (GetAsyncKeyState('Q') & 0x8000) != 0;
  const bool right = (GetAsyncKeyState('E') & 0x8000) != 0;
  if (frame.keys && step > 0.0 && step < 1.0 && left != right) {
    const float degrees = float(step) * kTurnDegreesPerSecond;
    session.turn_degrees += left ? degrees : -degrees;
    session.turn_degrees = std::fmod(session.turn_degrees, 360.0f);
  }
  if (frame.camera) {
    Spot spot = aim(frame.camera);
    spot.valid = allowed(spot, frame.player);
    session.spot = spot;
    session.has_spot = true;
    std::memcpy(session.preview.position, spot.position, sizeof(spot.position));
    std::memcpy(session.preview.rotation, spot.rotation, sizeof(spot.rotation));
    if (!session.preview.shown) {
      ghosts::show(session.preview);
    } else {
      ghosts::place(session.preview);
    }
    tint(spot.valid);
  }
  ghosts::draw_ring(session.preview);
  const bool cancel = session.cancel.pressed(frame.keys);
  const bool confirm = session.confirm.pressed(frame.keys);
  if (cancel) {
    reports.push_back(report_of(PlacerEvent::Cancel, nullptr));
    reports.push_back(report_of(PlacerEvent::End, nullptr));
    finish("cancelled");
    return;
  }
  // A client script's own session hears of a click its rules refused (local_previews.hpp).
  const bool local_session = (session.order.session & 0x80000000u) != 0;
  if (confirm && session.has_spot && !session.spot.valid && local_session) {
    reports.push_back(report_of(PlacerEvent::Rejected, &session.spot));
    return;
  }
  if (confirm && session.has_spot && session.spot.valid) {
    reports.push_back(report_of(PlacerEvent::Confirm, &session.spot));
    if (!(session.order.flags & kcd2s::integration::kPlacerKeepOpen)) {
      reports.push_back(report_of(PlacerEvent::End, nullptr));
      finish("confirmed");
    }
    return;
  }
  const bool report_moves = session.order.flags & kcd2s::integration::kPlacerReportMoves;
  const bool due = session.moved_at < 0 || frame.now - session.moved_at >= kMoveInterval;
  if (report_moves && session.has_spot && due && moved_enough(session.spot)) {
    reports.push_back(report_of(PlacerEvent::Move, &session.spot));
    session.reported = session.spot;
    session.has_reported = true;
    session.moved_at = frame.now;
  }
}

bool active() {
  return running;
}

bool rotate(float degrees) {
  if (!running) {
    return false;
  }
  session.turn_degrees = std::fmod(session.turn_degrees + degrees, 360.0f);
  return true;
}

bool refuse_action(const char* action, unsigned mode) {
  if (!running || !action || mode == kReleased) {
    return false;
  }
  for (const char* prefix : kKeptPrefixes) {
    if (std::strncmp(action, prefix, std::strlen(prefix)) == 0) {
      return false;
    }
  }
  return true;
}

void reset() {
  running = false;
  session = Session{};
}

}  // namespace kcds::runtime::props::placer
