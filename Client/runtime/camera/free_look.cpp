#include "free_look.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../physics/line_of_sight.hpp"
#include "../props/prop_entity.hpp"
#include "third_person.hpp"

namespace kcds::runtime::camera::free_look {
namespace {

constexpr char kAnchorName[] = "kcds_free_look";
constexpr char kHeadJoint[] = "Head";
// The camera sits this far in front of the head joint, along the view, and this far above it.
constexpr double kEyeForward = 0.12;
constexpr double kEyeUp = 0.06;
// How far the view turns from where the body faces (degrees), and the mouse's degrees per pixel.
constexpr double kYawLimit = 120;
constexpr double kPitchLimit = 70;
constexpr double kMouseDegrees = 0.12;
// The view from behind a seated player: this far behind a point this far above the head, and how
// far it looks down and up, starting a little down (metres, degrees).
constexpr double kBehindDistance = 4.8;
constexpr double kBehindAboveHead = 1.2;
constexpr double kBehindLowest = -60;
constexpr double kBehindHighest = 35;
constexpr double kBehindStart = -16;
constexpr double kAboveGround = 0.3;
// The look target lies this far along the view (metres).
constexpr double kTargetDistance = 10;
// The game services: + 8 the input module, whose + 224 is the action map manager;
// EnableActionMap(name, enable) is its + 232 (as NoClip).
constexpr std::size_t kServicesInput = 8;
constexpr std::size_t kActionMaps = 224;
constexpr std::size_t kEnableActionMap = 232;
constexpr char kPlayerActionMap[] = "player";
// CEntity GetWorldTM (vtable + 256): its columns are the entity's right, forward and up.
constexpr std::size_t kGetWorldTM = 256;
constexpr double kPi = 3.14159265358979323846;

using Get = void*(__fastcall*)(void*);
using EnableActionMap = void(__fastcall*)(void*, const char*, bool);
using GetWorldTM = const float*(__fastcall*)(void*);

FreeLookNatives natives;
Logger log_line = nullptr;
AnchorView view(kAnchorName);
double game_fov = 60;
// The view's heading and elevation, and the body's heading it is limited around (degrees).
double yaw = 0;
double pitch = 0;
double body_yaw = 0;
// The limits of the view now (set_limits), and whether a script gave them.
double yaw_limit = kYawLimit;
double up_limit = kPitchLimit;
double down_limit = kPitchLimit;
bool own_limits = false;
// The head's height over the body's place when the view began: the view from behind goes round
// that point over the body, not round the head, which a cutscene's clip swings about (a strike
// moved it half a metre, and the view shook with it).
double head_height = 1.6;
// Whether the game's player controls are held off now.
bool controls_held = false;
Pose placed;
std::optional<POINT> pointer_rest;

template <class Fn>
Fn slot(void* object, std::size_t offset) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[offset / sizeof(void*)]);
}

double radians(double degrees) {
  return degrees * kPi / 180;
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

void set_controls_held(bool hold) {
  if (hold == controls_held) {
    return;
  }
  controls_held = hold;
  void* services = natives.services ? natives.services() : nullptr;
  void* input =
      services ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + kServicesInput)
               : nullptr;
  void* maps = input ? slot<Get>(input, kActionMaps)(input) : nullptr;
  if (maps) {
    slot<EnableActionMap>(maps, kEnableActionMap)(maps, kPlayerActionMap, !hold);
  }
}

// Where the body faces (degrees from +Y, rising to the left).
double heading_of(void* entity) {
  const float* matrix = slot<GetWorldTM>(entity, kGetWorldTM)(entity);
  return std::atan2(-double(matrix[1]), double(matrix[5])) * 180 / kPi;
}

// One frame of the mouse: the pointer's move from the window's centre turns the view.
void turn_by_mouse() {
  double yaw_turn = 0;
  double pitch_turn = 0;
  if (mouse_turn(yaw_turn, pitch_turn)) {
    yaw += yaw_turn;
    pitch += pitch_turn;
  }
}

void stop(const char* why) {
  if (view.active()) {
    view.end();
  }
  set_controls_held(false);
  pointer_rest.reset();
  if (log_line) {
    log_line("FREE LOOK ends (%s)", why);
  }
}

}  // namespace

void install(const FreeLookNatives& given, Logger logger) {
  natives = given;
  log_line = logger;
}

// Behind the body along the view, above where its head was, in front of the ground. feet: the
// body's place. A client script's third person (Camera.setThirdPerson) sets how far behind and
// how high above the feet.
void place_behind(const float feet[3], const double forward[3]) {
  double distance = kBehindDistance;
  double above_feet = head_height + kBehindAboveHead;
  float script_distance = 0.0f;
  float script_height = 0.0f;
  if (third_person::script_view_seat(script_distance, script_height)) {
    distance = script_distance;
    above_feet = script_height;
  }
  const float pivot[3] = {feet[0], feet[1], float(feet[2] + above_feet)};
  const float reach[3] = {float(pivot[0] - forward[0] * distance),
                          float(pivot[1] - forward[1] * distance),
                          float(pivot[2] - forward[2] * distance)};
  placed.x = reach[0];
  placed.y = reach[1];
  placed.z = reach[2];
  physics::RayHit hit;
  const auto result = physics::trace(pivot, reach, physics::kHitTerrainOnly, nullptr, 0, hit);
  if (result == physics::TraceResult::Hit) {
    placed.z = (std::max)(double(placed.z), double(hit.position[2]) + kAboveGround);
  }
}

void keep(void* local_entity, bool wanted, bool in_world, bool hold_controls, bool behind,
          bool paused) {
  if (paused && in_world) {
    return;
  }
  const bool running = view.active();
  if (!wanted || !in_world || !local_entity) {
    if (running) {
      stop(!in_world ? "the world went" : "the request ended");
    }
    return;
  }
  float head[3] = {};
  float head_rotation[4] = {};
  if (!props::entity::joint_pose(local_entity, kHeadJoint, head, head_rotation)) {
    return;
  }
  if (!running) {
    Pose previous;
    if (!view.begin(natives.view, &previous, &game_fov)) {
      return;
    }
    body_yaw = heading_of(local_entity);
    float feet[3] = {};
    props::entity::world_position(local_entity, feet);
    head_height = std::clamp(double(head[2] - feet[2]), 0.5, 2.2);
    yaw = previous.yaw;
    pitch = behind ? kBehindStart : previous.pitch;
    // Limits of the script's own: the view begins where the body faces (the game's view could
    // look away from the board, live chess), in the middle of its up and down range.
    if (own_limits && !behind) {
      yaw = body_yaw;
      pitch = (up_limit - down_limit) / 2;
    }
    if (log_line) {
      log_line("FREE LOOK begins: body heading %.1f, view %.1f %.1f", body_yaw, yaw, pitch);
    }
  }
  set_controls_held(hold_controls);
  // The body was turned (a cart going round a bend): the view goes with it.
  const double heading = heading_of(local_entity);
  yaw += turn(body_yaw, heading);
  body_yaw = heading;
  turn_by_mouse();
  if (behind) {
    pitch = std::clamp(pitch, kBehindLowest, kBehindHighest);
  } else {
    yaw = body_yaw + std::clamp(turn(body_yaw, yaw), -yaw_limit, yaw_limit);
    pitch = std::clamp(pitch, -down_limit, up_limit);
  }
  const double forward[3] = {-std::sin(radians(yaw)) * std::cos(radians(pitch)),
                             std::cos(radians(yaw)) * std::cos(radians(pitch)),
                             std::sin(radians(pitch))};
  if (behind) {
    float feet[3] = {};
    props::entity::world_position(local_entity, feet);
    place_behind(feet, forward);
  } else {
    placed.x = head[0] + forward[0] * kEyeForward;
    placed.y = head[1] + forward[1] * kEyeForward;
    placed.z = head[2] + forward[2] * kEyeForward + kEyeUp;
  }
  placed.yaw = yaw;
  placed.pitch = pitch;
  placed.roll = 0;
  if (!view.place(placed, game_fov)) {
    stop("its view was taken away");
  }
}

void test_turn(double degrees) {
  yaw += degrees;
}

bool active() {
  return view.active();
}

bool mouse_turn(double& yaw_degrees, double& pitch_degrees) {
  yaw_degrees = 0;
  pitch_degrees = 0;
  void* window = natives.window ? natives.window() : nullptr;
  if (!window || GetForegroundWindow() != static_cast<HWND>(window) ||
      (natives.mouse_free && !natives.mouse_free())) {
    pointer_rest.reset();
    return false;
  }
  RECT client{};
  GetClientRect(static_cast<HWND>(window), &client);
  POINT centre{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
  ClientToScreen(static_cast<HWND>(window), &centre);
  POINT cursor{};
  const bool moved = pointer_rest && GetCursorPos(&cursor);
  if (moved) {
    yaw_degrees = -(cursor.x - pointer_rest->x) * kMouseDegrees;
    pitch_degrees = -(cursor.y - pointer_rest->y) * kMouseDegrees;
  }
  SetCursorPos(centre.x, centre.y);
  POINT rest{};
  if (GetCursorPos(&rest)) {
    pointer_rest = rest;
  } else {
    pointer_rest.reset();
  }
  return moved;
}

void forget_mouse() {
  pointer_rest.reset();
}

void set_limits(double yaw_degrees, double up, double down) {
  own_limits = yaw_degrees > 0;
  if (own_limits) {
    yaw_limit = std::clamp(yaw_degrees, 1.0, 180.0);
    up_limit = std::clamp(up, 0.0, 89.0);
    down_limit = std::clamp(down, 0.0, 89.0);
  } else {
    yaw_limit = kYawLimit;
    up_limit = kPitchLimit;
    down_limit = kPitchLimit;
  }
}

bool target(float out[3]) {
  if (!view.active()) {
    return false;
  }
  const double forward[3] = {-std::sin(radians(placed.yaw)) * std::cos(radians(placed.pitch)),
                             std::cos(radians(placed.yaw)) * std::cos(radians(placed.pitch)),
                             std::sin(radians(placed.pitch))};
  out[0] = static_cast<float>(placed.x + forward[0] * kTargetDistance);
  out[1] = static_cast<float>(placed.y + forward[1] * kTargetDistance);
  out[2] = static_cast<float>(placed.z + forward[2] * kTargetDistance);
  return true;
}

void reset() {
  if (view.active()) {
    view = AnchorView(kAnchorName);
    set_controls_held(false);
    pointer_rest.reset();
  }
}

}  // namespace kcds::runtime::camera::free_look
