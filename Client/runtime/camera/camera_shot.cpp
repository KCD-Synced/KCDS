#include "camera_shot.hpp"

#include "free_look.hpp"
#include "third_person.hpp"
#include "view_shake.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>
#include <utility>
#include <optional>

#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::camera {

namespace {

namespace integration = kcd2s::integration;
namespace sdk = kcd2s::sdk;

constexpr char kAnchorName[] = "kcds_camera_shot";
constexpr double kPi = 3.14159265358979323846;
// The most keys a camera path has (Client/resources/camera.cjs MAX_PATH_KEYS).
constexpr std::size_t kMaxPathKeys = 64;

ShotNatives natives;
AnchorView view(kAnchorName);
bool active = false;
// Whether the shot playing is a client script's (it ends back to the scripts, not the server).
bool local_shot = false;
// A client script's orders waiting for the engine thread, and how its last shot ended.
std::mutex local_mutex;
std::vector<std::optional<integration::CameraShot>> local_orders;
std::optional<integration::CameraShotEnd> local_ended;
// The game's own field of view when the shot began (degrees).
double game_fov = 60;
// The move in progress: from where the camera was to the shot.
Pose from_pose;
double from_fov = 60;
Pose to_pose;
double to_fov = 60;
double started_at = 0;
double length = 0;
// Whether the move starts and ends slowly; false: one speed throughout.
bool eased = true;
// A shot the mouse looks about in: how far to each side and up and down from where the
// shot looks (0 for a still shot), and how far the mouse has turned it.
double look_yaw = 0;
double look_up = 0;
double look_down = 0;
double looked_yaw = 0;
double looked_pitch = 0;

// A camera path (Camera.playPath): its keys, each a pose and field of view at
// a time from the path's start, the yaw and roll unwound so each key turns the short way from the
// one before. While it has keys, the view follows them instead of the move above: a smooth path
// through them (cubic Hermite with Catmull-Rom tangents over the keys' times, still at the first
// and last key), or straight lines at one speed between them. After the last key it holds there.
struct PathKey {
  Pose pose;
  double fov = 60;
  double time = 0;
};
std::vector<PathKey> path;
double path_started = 0;
bool path_smooth = true;
// Whether the scripts were told the path reached its last key.
bool path_finished = false;
// A client script's path waiting for the engine thread (the newest wins), and the notice that its
// path finished.
struct PathOrder {
  std::vector<PathKey> keys;
  bool smooth = true;
};
std::optional<PathOrder> local_path;
bool local_path_finished = false;

double degrees(double radians) {
  return radians * 180 / kPi;
}

// The heading (degrees from +Y, rising to the left) and elevation of a forward vector.
void aim_along(Pose& pose, double fx, double fy, double fz) {
  pose.yaw = degrees(std::atan2(-fx, fy));
  pose.pitch = std::clamp(degrees(std::atan2(fz, std::hypot(fx, fy))), -89.0, 89.0);
}

Pose pose_of(const integration::CameraShot& shot) {
  Pose pose;
  if (shot.aim == integration::CameraAim::LookAt) {
    aim_along(pose, double(shot.target[0]) - shot.position[0],
              double(shot.target[1]) - shot.position[1], double(shot.target[2]) - shot.position[2]);
    pose.roll = shot.roll;
  } else {
    // The quaternion turning +Y, with its roll.
    pose = pose_of_rotation(shot.rotation[0], shot.rotation[1], shot.rotation[2], shot.rotation[3]);
  }
  pose.x = shot.position[0];
  pose.y = shot.position[1];
  pose.z = shot.position[2];
  return pose;
}

// The shortest turn from one heading to another, in degrees.
double turn(double from, double to) {
  double difference = std::fmod(to - from, 360.0);
  if (difference > 180) {
    difference -= 360;
  } else if (difference < -180) {
    difference += 360;
  }
  return difference;
}

// One value of a path at `t` within key `i`'s segment: smooth or straight.
double path_value(const std::vector<double>& values, std::size_t i, double t) {
  const double t0 = path[i].time;
  const double t1 = path[i + 1].time;
  const double span = t1 - t0;
  const double s = span > 0 ? std::clamp((t - t0) / span, 0.0, 1.0) : 1.0;
  if (!path_smooth) {
    return values[i] + (values[i + 1] - values[i]) * s;
  }
  // Catmull-Rom tangents per second over the keys' times; still at the path's ends.
  const auto tangent = [&](std::size_t k) {
    if (k == 0 || k + 1 >= path.size()) {
      return 0.0;
    }
    const double across = path[k + 1].time - path[k - 1].time;
    return across > 0 ? (values[k + 1] - values[k - 1]) / across : 0.0;
  };
  const double s2 = s * s;
  const double s3 = s2 * s;
  const double h00 = 2 * s3 - 3 * s2 + 1;
  const double h10 = s3 - 2 * s2 + s;
  const double h01 = -2 * s3 + 3 * s2;
  const double h11 = s3 - s2;
  return h00 * values[i] + h10 * span * tangent(i) + h01 * values[i + 1] +
         h11 * span * tangent(i + 1);
}

// Where the path is at `seconds`; tells the scripts once it reaches its last key.
void path_pose_at(double seconds, Pose& pose, double& fov) {
  const double t = std::clamp(seconds - path_started, 0.0, path.back().time);
  std::size_t i = 0;
  while (i + 2 < path.size() && t > path[i + 1].time) {
    ++i;
  }
  std::vector<double> x, y, z, yaw, pitch, roll, view;
  for (const PathKey& key : path) {
    x.push_back(key.pose.x);
    y.push_back(key.pose.y);
    z.push_back(key.pose.z);
    yaw.push_back(key.pose.yaw);
    pitch.push_back(key.pose.pitch);
    roll.push_back(key.pose.roll);
    view.push_back(key.fov);
  }
  pose.x = path_value(x, i, t);
  pose.y = path_value(y, i, t);
  pose.z = path_value(z, i, t);
  pose.yaw = path_value(yaw, i, t);
  pose.pitch = std::clamp(path_value(pitch, i, t), -89.0, 89.0);
  pose.roll = path_value(roll, i, t);
  fov = std::clamp(path_value(view, i, t), 5.0, 150.0);
  if (!path_finished && seconds - path_started >= path.back().time) {
    path_finished = true;
    if (local_shot) {
      std::lock_guard<std::mutex> lock(local_mutex);
      local_path_finished = true;
    }
  }
}

// Where the move is at `seconds`, eased in and out.
void pose_at(double seconds, Pose& pose, double& fov) {
  if (!path.empty()) {
    path_pose_at(seconds, pose, fov);
    return;
  }
  double t = length > 0 ? std::clamp((seconds - started_at) / length, 0.0, 1.0) : 1.0;
  if (eased) {
    t = t * t * (3 - 2 * t);
  }
  pose.x = from_pose.x + (to_pose.x - from_pose.x) * t;
  pose.y = from_pose.y + (to_pose.y - from_pose.y) * t;
  pose.z = from_pose.z + (to_pose.z - from_pose.z) * t;
  pose.yaw = from_pose.yaw + turn(from_pose.yaw, to_pose.yaw) * t;
  pose.pitch = from_pose.pitch + (to_pose.pitch - from_pose.pitch) * t;
  pose.roll = from_pose.roll + turn(from_pose.roll, to_pose.roll) * t;
  fov = from_fov + (to_fov - from_fov) * t;
}

void report(integration::CameraShotEnd reason) {
  if (local_shot) {
    std::lock_guard<std::mutex> lock(local_mutex);
    local_ended = reason;
    return;
  }
  if (natives.send_ended) {
    natives.send_ended(reason);
  }
}

// A true field of a script's message.
bool flag(const sdk::Value& message, const char* name) {
  const sdk::Value* value = integration::script_field(message, name);
  return value && value->tag() == sdk::Tag::Bool && value->as_bool();
}

double number(const sdk::Value& message, const char* name, double fallback) {
  const sdk::Value* value = integration::script_field(message, name);
  if (!value) {
    return fallback;
  }
  if (value->tag() == sdk::Tag::Double) {
    return value->as_double();
  }
  if (value->tag() == sdk::Tag::Int) {
    return double(value->as_int());
  }
  return fallback;
}

// A point { x, y, z } of a script's message; false when it has none.
bool point(const sdk::Value& message, const char* name, float out[3]) {
  const sdk::Value* value = integration::script_field(message, name);
  if (!value || value->tag() != sdk::Tag::Map) {
    return false;
  }
  out[0] = float(number(*value, "x", 0));
  out[1] = float(number(*value, "y", 0));
  out[2] = float(number(*value, "z", 0));
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// The pose of a script's shot or path key: { position, lookAt and roll | rotation, fov }; false
// for a malformed one.
bool shot_of(const sdk::Value& message, integration::CameraShot& shot) {
  if (!point(message, "position", shot.position)) {
    return false;
  }
  if (point(message, "lookAt", shot.target)) {
    shot.aim = integration::CameraAim::LookAt;
    shot.roll = float(number(message, "roll", 0));
  } else {
    const sdk::Value* rotation = integration::script_field(message, "rotation");
    if (!rotation || rotation->tag() != sdk::Tag::Map) {
      return false;
    }
    shot.aim = integration::CameraAim::Rotation;
    shot.rotation[0] = float(number(*rotation, "x", 0));
    shot.rotation[1] = float(number(*rotation, "y", 0));
    shot.rotation[2] = float(number(*rotation, "z", 0));
    shot.rotation[3] = float(number(*rotation, "w", 1));
  }
  shot.fov = float(std::clamp(number(message, "fov", 0), 0.0, 150.0));
  return true;
}

void begin_shot(const integration::CameraShot& shot, double seconds) {
  if (active) {
    // The move starts from wherever the shot before has got to.
    pose_at(seconds, from_pose, from_fov);
  } else {
    if (!view.begin(natives.view, &from_pose, &game_fov)) {
      view = AnchorView(kAnchorName);
      report(integration::CameraShotEnd::Unavailable);
      return;
    }
    from_fov = game_fov;
    active = true;
  }
  to_pose = pose_of(shot);
  to_fov = shot.fov > 0 ? shot.fov : game_fov;
  started_at = seconds;
  length = shot.seconds;
  eased = shot.ease;
  look_yaw = shot.look_yaw;
  look_up = shot.look_up;
  look_down = shot.look_down;
  looked_yaw = 0;
  looked_pitch = 0;
  free_look::forget_mouse();
  path.clear();
}

// The mouse turns a shot with look, within its limits.
void look_about(Pose& pose) {
  if (look_yaw <= 0 || !path.empty()) {
    return;
  }
  double yaw_turn = 0;
  double pitch_turn = 0;
  if (free_look::mouse_turn(yaw_turn, pitch_turn)) {
    looked_yaw = std::clamp(looked_yaw + yaw_turn, -look_yaw, look_yaw);
    looked_pitch = std::clamp(looked_pitch + pitch_turn, -look_down, look_up);
  }
  pose.yaw += looked_yaw;
  pose.pitch = std::clamp(pose.pitch + looked_pitch, -89.0, 89.0);
}

// A script's path: from wherever the camera is when its first key comes later than its start.
void begin_path(const PathOrder& order, double seconds) {
  Pose current;
  double current_fov = game_fov;
  if (active) {
    pose_at(seconds, current, current_fov);
  } else {
    if (!view.begin(natives.view, &current, &game_fov)) {
      view = AnchorView(kAnchorName);
      report(integration::CameraShotEnd::Unavailable);
      return;
    }
    current_fov = game_fov;
    active = true;
  }
  std::vector<PathKey> keys;
  if (order.keys.front().time > 0) {
    keys.push_back(PathKey{current, current_fov, 0});
  }
  for (PathKey key : order.keys) {
    if (key.fov <= 0) {
      key.fov = game_fov;
    }
    // Each heading and roll the short way round from the key before.
    if (!keys.empty()) {
      key.pose.yaw = keys.back().pose.yaw + turn(keys.back().pose.yaw, key.pose.yaw);
      key.pose.roll = keys.back().pose.roll + turn(keys.back().pose.roll, key.pose.roll);
    }
    keys.push_back(key);
  }
  path = std::move(keys);
  path_started = seconds;
  path_smooth = order.smooth;
  path_finished = false;
}

void end_shot() {
  if (active) {
    view.end();
  }
  active = false;
  path.clear();
}

void play(const std::optional<integration::CameraShot>& order, bool in_world, double seconds,
          bool local) {
  if (!order) {
    // A clear ends only a shot of the same side.
    if (local_shot == local) {
      end_shot();
    }
  } else if (in_world) {
    // Whose shot it is before it begins: a view that cannot be made is told to that side.
    local_shot = local;
    begin_shot(*order, seconds);
  }
}

}  // namespace

void install_shots(const ShotNatives& given) {
  natives = given;
}

bool shot_active() {
  return active;
}

void tick_shots(bool in_world, double seconds,
                const std::vector<std::optional<integration::CameraShot>>& orders) {
  if (active && !in_world) {
    // The view system lost our view with the world; nothing is left to remove.
    view = AnchorView(kAnchorName);
    active = false;
    report(integration::CameraShotEnd::WorldChanged);
  }
  // A script's view shakes (Camera.shake) on whichever view is active, the game's or a shot's.
  shake::tick(natives.view.framework ? natives.view.framework() : nullptr, in_world);
  for (const std::optional<integration::CameraShot>& order : orders) {
    play(order, in_world, seconds, false);
  }
  std::vector<std::optional<integration::CameraShot>> local;
  {
    std::lock_guard<std::mutex> lock(local_mutex);
    local.swap(local_orders);
  }
  for (const std::optional<integration::CameraShot>& order : local) {
    play(order, in_world, seconds, true);
  }
  std::optional<PathOrder> path_order;
  {
    std::lock_guard<std::mutex> lock(local_mutex);
    path_order.swap(local_path);
  }
  if (path_order && in_world) {
    local_shot = true;
    begin_path(*path_order, seconds);
  }
  if (!active) {
    return;
  }
  Pose pose;
  double fov = game_fov;
  pose_at(seconds, pose, fov);
  look_about(pose);
  if (!view.place(pose, fov)) {
    view.end();
    view = AnchorView(kAnchorName);
    active = false;
    report(integration::CameraShotEnd::Lost);
  }
}

bool handle_runtime_message(const sdk::Value& message) {
  if (shake::handle_runtime_message(message)) {
    return true;
  }
  const std::string operation = integration::script_operation(message);
  if (operation == "cameraThirdPerson") {
    // Camera.setThirdPerson; the script's checks ran in Client/resources/camera.cjs.
    third_person::set_script_view(flag(message, "on"), float(number(message, "distance", 2.4)),
                                  float(number(message, "height", 1.6)),
                                  float(number(message, "shoulder", 0.45)));
    return true;
  }
  if (operation == "cameraClearLocal") {
    std::lock_guard<std::mutex> lock(local_mutex);
    local_orders.push_back(std::nullopt);
    local_path.reset();
    return true;
  }
  if (operation == "cameraPathLocal") {
    // Camera.playPath; the script's checks ran in Client/resources/camera.cjs.
    PathOrder order;
    order.smooth = !integration::script_field(message, "smooth") || flag(message, "smooth");
    const sdk::Value* keys = integration::script_field(message, "keys");
    if (!keys || keys->tag() != sdk::Tag::Array) {
      return true;
    }
    double last = -1;
    for (const sdk::Value& key : keys->as_array()) {
      PathKey made;
      integration::CameraShot shot;
      if (!shot_of(key, shot)) {
        return true;
      }
      made.pose = pose_of(shot);
      made.fov = shot.fov;
      made.time = std::clamp(number(key, "time", 0), 0.0, 600.0);
      if (made.time <= last) {
        return true;
      }
      last = made.time;
      order.keys.push_back(made);
    }
    if (order.keys.size() < 2 || order.keys.size() > kMaxPathKeys) {
      return true;
    }
    std::lock_guard<std::mutex> lock(local_mutex);
    local_path = std::move(order);
    return true;
  }
  if (operation != "cameraShotLocal") {
    return false;
  }
  // The script's checks ran in Client/resources/camera.cjs; a malformed message is dropped.
  integration::CameraShot shot;
  if (!shot_of(message, shot)) {
    return true;
  }
  shot.seconds = float(std::clamp(number(message, "seconds", 0), 0.0, 60.0));
  const sdk::Value* ease = integration::script_field(message, "ease");
  shot.ease = !ease || ease->tag() != sdk::Tag::Bool || ease->as_bool();
  std::lock_guard<std::mutex> lock(local_mutex);
  local_orders.push_back(shot);
  return true;
}

void pump(const RuntimeSender& send) {
  std::optional<integration::CameraShotEnd> ended;
  bool finished = false;
  {
    std::lock_guard<std::mutex> lock(local_mutex);
    ended.swap(local_ended);
    finished = std::exchange(local_path_finished, false);
  }
  if (finished) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("cameraPathLocalFinished"))}}));
  }
  if (!ended) {
    return;
  }
  const char* reason = *ended == integration::CameraShotEnd::WorldChanged  ? "worldChanged"
                       : *ended == integration::CameraShotEnd::Unavailable ? "unavailable"
                                                                           : "lost";
  send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("cameraShotLocalEnded"))},
                                  {"reason", sdk::Value(std::string(reason))}}));
}

}  // namespace kcds::runtime::camera
