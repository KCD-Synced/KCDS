#include "free_camera.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../camera/anchor_view.hpp"
#include "../physics/line_of_sight.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::noclip {
namespace {

namespace sdk = kcd2s::sdk;

// CEntity: SetWorldTM(matrix, reason), GetWorldTM; its physics component's EnablePhysics(bool).
constexpr std::size_t kSetWorldTM = 240;
constexpr std::size_t kGetWorldTM = 256;
// Landing a body where the flight ended (land_on_ground, metres): the ray starts a little above
// its feet and looks this far down; failing that, from high above for a body under the
// terrain; the feet end this high over the surface.
constexpr float kLandProbeAbove = 0.5f;
constexpr float kLandProbeBelow = 1000.0f;
constexpr float kLandFromSky = 500.0f;
constexpr float kLandAbove = 0.05f;
constexpr std::size_t kEnablePhysics = 192;
// The body hangs this far under the camera, clear of the view when it looks down.
constexpr double kBodyBelowCamera = 2.2;
// The game services: + 8 the input module, whose + 224 is the action map manager;
// EnableActionMap(name, enable) is its + 232.
constexpr std::size_t kServicesInput = 8;
constexpr std::size_t kActionMaps = 224;
constexpr std::size_t kEnableActionMap = 232;
constexpr char kPlayerActionMap[] = "player";

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinSpeed = 0.05;
constexpr double kMaxSpeed = 400;
constexpr double kBoost = 6;
constexpr double kCrawl = 5;
constexpr double kMaxStep = 0.1;
constexpr double kMouseDegrees = 0.12;
// An orbit's distance (metres), and how far in front of what is solid the camera stays.
constexpr double kOrbitNearest = 0.5;
constexpr double kOrbitFarthest = 50;
constexpr double kOrbitClearance = 0.25;
// The ground test starts this far out from the target: a body orbited (its own parts) is no
// obstacle (a ray from the chest hit the player at once and held the camera 0.25 m away).
constexpr double kOrbitRayStart = 0.6;

// The photo mode's keys by their place on the keyboard (scan codes).
constexpr unsigned kForward = 0x11;  // W
constexpr unsigned kBack = 0x1F;     // S
constexpr unsigned kLeft = 0x1E;     // A
constexpr unsigned kRight = 0x20;    // D
constexpr unsigned kRise = 0x39;     // Space
constexpr unsigned kSink = 0x2E;     // C
constexpr unsigned kFast = 0x2A;     // left Shift
constexpr unsigned kSlow = 0x38;     // left Alt

using Get = void*(__fastcall*)(void* object);
using EnableActionMap = void(__fastcall*)(void* maps, const char* name, bool enable);
using SetWorldTM = void(__fastcall*)(void* entity, const float* matrix, unsigned reason);
using GetWorldTM = const float*(__fastcall*)(void* entity);
// The physics component getter returns a std::shared_ptr: the pointer and its control block.
struct SharedPointer {
  void* object;
  void* control;
};
using PhysicsComponent = SharedPointer*(__fastcall*)(void* entity, SharedPointer* out, int create);
using EnablePhysics = void(__fastcall*)(void* component, bool enable);

using camera::Pose;

struct Move {
  double forward = 0;
  double strafe = 0;
  double lift = 0;
  double yaw = 0;
  double pitch = 0;
  bool boost = false;
  bool crawl = false;
  double dt = 0;
};

struct Settings {
  bool body = true;
  // The player sees their own body during the flight (showBody): a camera mode that
  // looks at the player from the front, as an animation menu does.
  bool show_body = false;
  // Orbit: the camera circles target at distance, looking at it; the mouse turns it
  // around, W/S bring it nearer or farther. With keep_above_ground it stays in front of what is
  // solid between the target and it (the ground, a wall).
  bool orbit = false;
  double orbit_target[3] = {0, 0, 0};
  double orbit_distance = 3;
  bool keep_above_ground = true;
  bool input = true;
  // The built-in driver turns and moves only while the right mouse button is held (look:
  // 'rightButton'): a focused page keeps the pointer and the keys otherwise.
  bool right_button = false;
  double speed = 12;
  double fov = 0;  // degrees; 0 keeps the game's
};

Logger log_line = nullptr;
Natives natives;

// Requests from the runtime.
std::mutex request_mutex;
bool enable_requested = false;
bool disable_requested = false;
std::optional<Settings> requested_settings;
std::optional<Pose> requested_pose;
std::optional<Move> requested_move;

// Engine thread only.
bool flying = false;
camera::AnchorView view("kcds_noclip_camera");
// Body mode: the player's entity, and its world matrix when the flight began.
void* body = nullptr;
float body_start[12] = {};
bool keep_position = true;
Pose pose;
Settings settings;
double game_fov = 0;  // degrees
double last_seconds = 0;
// Where the pointer really stood after the last re-centring: SetCursorPos may not reach the
// window's centre (another window's ClipCursor, live: three clients on one desktop), and a turn
// measured from the centre then came back every frame and spun the camera on the spot. The next
// turn is measured from here.
std::optional<POINT> pointer_rest;

// The state for the runtime.
std::mutex state_mutex;
sdk::Value::Map state;
bool state_changed = false;
std::optional<std::string> ended;

// Whether the server lets the local player fly, and whether the runtime has to hear
// it: after a change, and once per runtime session.
std::atomic<bool> allowed{true};
bool allowed_changed = false;
const void* told_session = nullptr;

template <class Fn>
Fn slot(void* object, std::size_t offset) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[offset / sizeof(void*)]);
}

double radians(double degrees) {
  return degrees * kPi / 180;
}

double degrees(double radians) {
  return radians * 180 / kPi;
}

double wrap_yaw(double yaw) {
  yaw = std::fmod(yaw + 180, 360);
  return yaw < 0 ? yaw + 180 : yaw - 180;
}

double clamp_pitch(double pitch) {
  return std::clamp(pitch, -89.0, 89.0);
}

void hold_controls(bool hold) {
  void* services = natives.services ? natives.services() : nullptr;
  void* input =
      services ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + kServicesInput)
               : nullptr;
  void* maps = input ? slot<Get>(input, kActionMaps)(input) : nullptr;
  if (maps) {
    slot<EnableActionMap>(maps, kEnableActionMap)(maps, kPlayerActionMap, !hold);
  }
}

// Releases a shared_ptr the game handed out (its control block's use count at + 8).
void release(const SharedPointer& pointer) {
  if (!pointer.control) {
    return;
  }
  auto counts = reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(pointer.control) + 8);
  if (_InterlockedExchangeAdd(counts, -1) == 1) {
    void** vtable = *static_cast<void***>(pointer.control);
    reinterpret_cast<void(__fastcall*)(void*)>(vtable[0])(pointer.control);
    if (_InterlockedExchangeAdd(counts + 1, -1) == 1) {
      reinterpret_cast<void(__fastcall*)(void*)>(vtable[1])(pointer.control);
    }
  }
}

void set_physics(void* entity, bool enable) {
  if (!entity || !natives.physics_component) {
    return;
  }
  SharedPointer component{};
  reinterpret_cast<PhysicsComponent>(natives.physics_component)(entity, &component, 1);
  if (component.object) {
    slot<EnablePhysics>(component.object, kEnablePhysics)(component.object, enable);
  }
  release(component);
}

// The body under the camera, upright, facing the camera's heading.
void place_body(const Pose& at) {
  const double yaw = radians(at.yaw);
  const float c = static_cast<float>(std::cos(yaw));
  const float s = static_cast<float>(std::sin(yaw));
  const float matrix[12] = {c, -s, 0, static_cast<float>(at.x),
                            s, c,  0, static_cast<float>(at.y),
                            0, 0,  1, static_cast<float>(at.z - kBodyBelowCamera)};
  slot<SetWorldTM>(body, kSetWorldTM)(body, matrix, 0);
}

bool key_down(HKL layout, unsigned scan) {
  const UINT virtual_key = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, layout);
  return virtual_key && (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

// Moves the pose by one frame of flight: forward and strafe along the heading, lift along world
// up, the turn in degrees.
void fly(const Move& move) {
  const double dt = std::min(move.dt, kMaxStep);
  double speed = settings.speed;
  if (move.boost) {
    speed *= kBoost;
  }
  if (move.crawl) {
    speed /= kCrawl;
  }
  pose.yaw = wrap_yaw(pose.yaw + move.yaw);
  pose.pitch = clamp_pitch(pose.pitch + move.pitch);
  const double yaw = radians(pose.yaw);
  const double pitch = radians(pose.pitch);
  const double forward[3] = {-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch),
                             std::sin(pitch)};
  const double right[3] = {std::cos(yaw), std::sin(yaw), 0};
  const double step = speed * dt;
  if (settings.orbit) {
    // Forward and back bring the orbiting camera nearer or farther; the frame places it.
    settings.orbit_distance =
        std::clamp(settings.orbit_distance - move.forward * step, kOrbitNearest, kOrbitFarthest);
    return;
  }
  pose.x += (forward[0] * move.forward + right[0] * move.strafe) * step;
  pose.y += (forward[1] * move.forward + right[1] * move.strafe) * step;
  pose.z += (forward[2] * move.forward + move.lift) * step;
}

// The orbiting camera behind its target along the view; with keep_above_ground it
// comes in front of the first solid thing between the target and that place.
void place_on_orbit() {
  const double yaw = radians(pose.yaw);
  const double pitch = radians(pose.pitch);
  const double forward[3] = {-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch),
                             std::sin(pitch)};
  double distance = settings.orbit_distance;
  if (settings.keep_above_ground && distance > kOrbitRayStart) {
    const float from[3] = {
        static_cast<float>(settings.orbit_target[0] - forward[0] * kOrbitRayStart),
        static_cast<float>(settings.orbit_target[1] - forward[1] * kOrbitRayStart),
        static_cast<float>(settings.orbit_target[2] - forward[2] * kOrbitRayStart)};
    const float to[3] = {static_cast<float>(settings.orbit_target[0] - forward[0] * distance),
                         static_cast<float>(settings.orbit_target[1] - forward[1] * distance),
                         static_cast<float>(settings.orbit_target[2] - forward[2] * distance)};
    float hit = 0;
    if (physics::first_hit(from, to, hit)) {
      distance = std::max(kOrbitNearest * 0.5,
                          kOrbitRayStart + static_cast<double>(hit) - kOrbitClearance);
    }
  }
  pose.x = settings.orbit_target[0] - forward[0] * distance;
  pose.y = settings.orbit_target[1] - forward[1] * distance;
  pose.z = settings.orbit_target[2] - forward[2] * distance;
}

// One frame of the built-in driver: the keys and the mouse, the pointer kept in the window.
void fly_by_input(double dt) {
  void* window = natives.window ? natives.window() : nullptr;
  if (!window || GetForegroundWindow() != static_cast<HWND>(window)) {
    pointer_rest.reset();
    return;
  }
  // The button's physical state: a focused page that takes its clicks does not hide it.
  if (settings.right_button && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0) {
    pointer_rest.reset();
    return;
  }
  HKL layout = GetKeyboardLayout(0);
  Move move;
  move.forward = (key_down(layout, kForward) ? 1 : 0) - (key_down(layout, kBack) ? 1 : 0);
  move.strafe = (key_down(layout, kRight) ? 1 : 0) - (key_down(layout, kLeft) ? 1 : 0);
  move.lift = (key_down(layout, kRise) ? 1 : 0) - (key_down(layout, kSink) ? 1 : 0);
  move.boost = key_down(layout, kFast);
  move.crawl = key_down(layout, kSlow);
  RECT client{};
  GetClientRect(static_cast<HWND>(window), &client);
  POINT centre{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
  ClientToScreen(static_cast<HWND>(window), &centre);
  POINT cursor{};
  if (pointer_rest && GetCursorPos(&cursor)) {
    move.yaw = -(cursor.x - pointer_rest->x) * kMouseDegrees;
    move.pitch = -(cursor.y - pointer_rest->y) * kMouseDegrees;
  }
  SetCursorPos(centre.x, centre.y);
  POINT rest{};
  if (GetCursorPos(&rest)) {
    pointer_rest = rest;
  } else {
    pointer_rest.reset();
  }
  move.dt = dt;
  fly(move);
}

void report(bool active) {
  double rendered = settings.fov > 0 ? settings.fov : game_fov;
  double aspect = 0;
  if (void* window = natives.window ? natives.window() : nullptr) {
    RECT client{};
    GetClientRect(static_cast<HWND>(window), &client);
    if (client.bottom > client.top) {
      aspect = static_cast<double>(client.right - client.left) / (client.bottom - client.top);
    }
  }
  sdk::Value::Map next{{"op", sdk::Value(std::string("noclipState"))},
                       {"active", sdk::Value(active)},
                       {"input", sdk::Value(settings.input)},
                       {"x", sdk::Value(pose.x)},
                       {"y", sdk::Value(pose.y)},
                       {"z", sdk::Value(pose.z)},
                       {"yaw", sdk::Value(pose.yaw)},
                       {"pitch", sdk::Value(pose.pitch)},
                       {"speed", sdk::Value(settings.speed)},
                       {"fov", sdk::Value(settings.fov > 0 ? settings.fov : game_fov)},
                       {"renderedFov", sdk::Value(rendered)},
                       {"aspect", sdk::Value(aspect)}};
  std::lock_guard<std::mutex> lock(state_mutex);
  state = std::move(next);
  state_changed = true;
}

bool begin() {
  camera::Natives camera_natives;
  camera_natives.framework = natives.framework;
  camera_natives.entity_system = natives.entity_system;
  camera_natives.spawn_params_init = natives.spawn_params_init;
  if (!view.begin(camera_natives, &pose, &game_fov)) {
    return false;
  }
  view.place(pose, settings.fov > 0 ? settings.fov : game_fov);
  hold_controls(true);
  body = settings.body && natives.local_entity ? natives.local_entity() : nullptr;
  if (body) {
    std::memcpy(body_start, slot<GetWorldTM>(body, kGetWorldTM)(body), sizeof(body_start));
    set_physics(body, false);
  }
  flying = true;
  pointer_rest.reset();
  if (log_line) {
    log_line("NOCLIP enabled at %.2f %.2f %.2f", pose.x, pose.y, pose.z);
  }
  return true;
}

// Puts a body that lands where the flight ended on the first surface below it (terrain, a floor,
// a roof), so it does not fall when its physics comes back: a fall made the character grunt or
// cry out and could hurt ("Henry says something" after the free camera). A
// body under the terrain goes up onto it. False when no surface is found (it stays where it is).
bool land_on_ground(void* entity) {
  float matrix[12];
  std::memcpy(matrix, slot<GetWorldTM>(entity, kGetWorldTM)(entity), sizeof(matrix));
  const float x = matrix[3];
  const float y = matrix[7];
  const float z = matrix[11];
  float distance = 0.0f;
  float ground = 0.0f;
  const float from[3] = {x, y, z + kLandProbeAbove};
  const float to[3] = {x, y, z - kLandProbeBelow};
  const float from_sky[3] = {x, y, z + kLandFromSky};
  if (physics::first_hit(from, to, distance)) {
    ground = from[2] - distance;
  } else if (physics::first_hit(from_sky, to, distance)) {
    ground = from_sky[2] - distance;
  } else {
    return false;
  }
  matrix[11] = ground + kLandAbove;
  slot<SetWorldTM>(entity, kSetWorldTM)(entity, matrix, 0);
  if (log_line) {
    log_line("NOCLIP landed at %.2f %.2f %.2f (was %.2f)", x, y, matrix[11], z);
  }
  return true;
}

void end() {
  view.end();
  if (body) {
    if (!keep_position) {
      slot<SetWorldTM>(body, kSetWorldTM)(body, body_start, 0);
    } else {
      land_on_ground(body);
    }
    set_physics(body, true);
    body = nullptr;
  }
  hold_controls(false);
  flying = false;
  if (log_line) {
    log_line("NOCLIP disabled");
  }
}

double number(const sdk::Value& message, const char* name, double fallback) {
  const sdk::Value* value = kcd2s::integration::script_field(message, name);
  if (value && value->tag() == sdk::Tag::Double) {
    return value->as_double();
  }
  if (value && value->tag() == sdk::Tag::Int) {
    return static_cast<double>(value->as_int());
  }
  return fallback;
}

bool flag(const sdk::Value& message, const char* name, bool fallback) {
  const sdk::Value* value = kcd2s::integration::script_field(message, name);
  return value && value->tag() == sdk::Tag::Bool ? value->as_bool() : fallback;
}

// What a flight is started with and keeps (its mode and orbit) from `flight`; the rest of `next`
// (speed, input, look, field of view) stays.
void keep_flight(Settings& next, const Settings& flight) {
  next.body = flight.body;
  next.show_body = flight.show_body;
  next.orbit = flight.orbit;
  std::copy(std::begin(flight.orbit_target), std::end(flight.orbit_target),
            std::begin(next.orbit_target));
  next.orbit_distance = flight.orbit_distance;
  next.keep_above_ground = flight.keep_above_ground;
}

Settings settings_of(const sdk::Value& message) {
  Settings next;
  const sdk::Value* mode = kcd2s::integration::script_field(message, "mode");
  next.body = !(mode && mode->tag() == sdk::Tag::Str && mode->as_str() == "camera");
  next.show_body = flag(message, "showBody", false);
  const sdk::Value* orbit = kcd2s::integration::script_field(message, "orbit");
  if (orbit && orbit->tag() == sdk::Tag::Map) {
    const sdk::Value* target = kcd2s::integration::script_field(*orbit, "target");
    if (target && target->tag() == sdk::Tag::Map) {
      next.orbit = true;
      next.orbit_target[0] = number(*target, "x", 0);
      next.orbit_target[1] = number(*target, "y", 0);
      next.orbit_target[2] = number(*target, "z", 0);
      next.orbit_distance =
          std::clamp(number(*orbit, "distance", 3), kOrbitNearest, kOrbitFarthest);
    }
  }
  next.keep_above_ground = flag(message, "keepAboveGround", true);
  next.input = flag(message, "input", true);
  const sdk::Value* look = kcd2s::integration::script_field(message, "look");
  next.right_button = look && look->tag() == sdk::Tag::Str && look->as_str() == "rightButton";
  next.speed = std::clamp(number(message, "speed", 12), kMinSpeed, kMaxSpeed);
  const double fov = number(message, "fov", 0);
  next.fov = fov > 0 ? std::clamp(fov, 20.0, 140.0) : 0;
  return next;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  return natives.framework && natives.services && natives.window && natives.entity_system &&
         natives.spawn_params_init && natives.physics_component && natives.local_entity;
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  std::lock_guard<std::mutex> lock(request_mutex);
  if (operation == "noclipEnable") {
    enable_requested = true;
    disable_requested = false;
    requested_settings = settings_of(message);
    return true;
  }
  if (operation == "noclipDisable") {
    disable_requested = true;
    keep_position = flag(message, "keepPosition", true);
    enable_requested = false;
    return true;
  }
  if (operation == "noclipSettings") {
    Settings next = settings_of(message);
    // A settings change (speed, input, look, field of view) has no mode: when it comes before
    // the frame took a waiting enable (NoClip.enable then setSpeed at once), the enable's flight
    // (camera or body, orbit) stays, or the camera mode would start as a body flight.
    if (requested_settings) {
      keep_flight(next, *requested_settings);
    }
    requested_settings = next;
    return true;
  }
  if (operation == "noclipPose") {
    Pose next;
    next.x = number(message, "x", 0);
    next.y = number(message, "y", 0);
    next.z = number(message, "z", 0);
    next.yaw = wrap_yaw(number(message, "yaw", 0));
    next.pitch = clamp_pitch(number(message, "pitch", 0));
    if (std::isfinite(next.x) && std::isfinite(next.y) && std::isfinite(next.z)) {
      requested_pose = next;
    }
    return true;
  }
  if (operation == "noclipMove") {
    Move move;
    move.forward = std::clamp(number(message, "forward", 0), -1.0, 1.0);
    move.strafe = std::clamp(number(message, "strafe", 0), -1.0, 1.0);
    move.lift = std::clamp(number(message, "lift", 0), -1.0, 1.0);
    move.yaw = number(message, "yaw", 0);
    move.pitch = number(message, "pitch", 0);
    move.boost = flag(message, "boost", false);
    move.crawl = flag(message, "crawl", false);
    move.dt = std::clamp(number(message, "dt", 0), 0.0, kMaxStep);
    requested_move = move;
    return true;
  }
  return false;
}

void frame(bool in_world, double seconds) {
  const double dt = last_seconds > 0 ? std::min(seconds - last_seconds, kMaxStep) : 0;
  last_seconds = seconds;
  bool enable = false;
  bool disable = false;
  std::optional<Settings> next_settings;
  std::optional<Pose> next_pose;
  std::optional<Move> next_move;
  {
    std::lock_guard<std::mutex> lock(request_mutex);
    enable = enable_requested;
    disable = disable_requested;
    enable_requested = disable_requested = false;
    next_settings.swap(requested_settings);
    next_pose.swap(requested_pose);
    next_move.swap(requested_move);
  }
  if (flying && !in_world) {
    flying = false;
    view = camera::AnchorView("kcds_noclip_camera");
    body = nullptr;
    hold_controls(false);
    std::lock_guard<std::mutex> lock(state_mutex);
    ended = "worldChanged";
    return;
  }
  // The server forbade NoClip: no flight starts and one that runs ends.
  if (!allowed.load()) {
    if (enable || flying) {
      if (flying) {
        end();
      }
      std::lock_guard<std::mutex> lock(state_mutex);
      ended = "notAllowed";
    }
    return;
  }
  if (next_settings) {
    const Settings flight = settings;
    settings = *next_settings;
    if (flying) {
      // What the flight was started with lasts for it.
      keep_flight(settings, flight);
    }
  }
  if (disable && flying) {
    end();
    report(false);
    return;
  }
  if (enable && !flying && !begin()) {
    std::lock_guard<std::mutex> lock(state_mutex);
    ended = "noCamera";
    return;
  }
  if (!flying) {
    return;
  }
  if (next_pose) {
    pose = *next_pose;
  }
  if (next_move && !settings.input) {
    fly(*next_move);
  }
  if (settings.input) {
    fly_by_input(dt);
  }
  if (settings.orbit) {
    place_on_orbit();
  }
  if (!view.place(pose, settings.fov > 0 ? settings.fov : game_fov)) {
    end();
    std::lock_guard<std::mutex> lock(state_mutex);
    ended = "worldChanged";
    return;
  }
  if (body) {
    place_body(pose);
  }
  report(true);
}

void pump(const void* session, const RuntimeSender& send) {
  std::optional<sdk::Value::Map> next;
  std::optional<std::string> reason;
  bool tell_allowed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (state_changed) {
      next = state;
      state_changed = false;
    }
    reason.swap(ended);
    if (allowed_changed || session != told_session) {
      tell_allowed = true;
      allowed_changed = false;
      told_session = session;
    }
  }
  if (next) {
    send(sdk::Value(std::move(*next)));
  }
  if (reason) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("noclipEnded"))},
                                    {"reason", sdk::Value(*reason)}}));
  }
  if (tell_allowed) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("noclipAllowed"))},
                                    {"allowed", sdk::Value(allowed.load())}}));
  }
}

void set_allowed(bool value) {
  if (allowed.exchange(value) == value) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  allowed_changed = true;
}

bool active() {
  return flying;
}

bool steers_pointer() {
  return flying && settings.input && !settings.right_button;
}

bool camera_view(camera::Pose& out, double& fov_degrees, double& aspect) {
  if (!flying) {
    return false;
  }
  out = pose;
  fov_degrees = settings.fov > 0 ? settings.fov : game_fov;
  aspect = 16.0 / 9.0;
  if (void* window = natives.window ? natives.window() : nullptr) {
    RECT client{};
    GetClientRect(static_cast<HWND>(window), &client);
    if (client.bottom > client.top) {
      aspect = static_cast<double>(client.right - client.left) / (client.bottom - client.top);
    }
  }
  return true;
}

bool hides_body() {
  return flying && !settings.show_body;
}

void abandon() {
  if (!flying) {
    return;
  }
  if (log_line) {
    log_line("NOCLIP ended: the engine adapter stopped");
  }
  keep_position = true;
  end();
}

}  // namespace kcds::runtime::noclip
