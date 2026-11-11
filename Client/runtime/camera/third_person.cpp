#include "third_person.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "../core/game_layouts.hpp"
#include "../physics/line_of_sight.hpp"
#include "../props/prop_entity.hpp"
#include "../keybinds/bound_keys.hpp"
#include "../keybinds/keybinds.hpp"
#include "MinHook.h"
#include "view_steps.hpp"

namespace kcds::runtime::camera::third_person {
namespace {

namespace view_steps = kcds::runtime::camera::view_steps;
namespace keybinds = kcds::runtime::keybinds;

// The view key's action in the game's key bindings.
constexpr const char* kViewAction = "kcds_third_person";

// The services object's camera manager.
constexpr std::size_t kServicesCameraManager = 56;
// The camera manager: the view (0 first person, 1 third person) and the dirty flag.
constexpr std::size_t kManagerCamera = 40;
constexpr std::size_t kManagerThirdPerson = 56;
constexpr std::size_t kManagerDirty = 58;
// Set while the game films something else through the manager (its view update then takes
// another path).
constexpr std::size_t kManagerOtherView = 59;
// The local player: the services' game interface (+ 8) slot 64 is the actor, whose entity is at
// + 56 (core/frame.cpp).
constexpr std::size_t kServicesGameInterface = 8;
constexpr std::size_t kLocalActorSlot = 64;
constexpr std::size_t kActorEntity = kcds::layouts::Actor::kEntity;
// Where the camera stands (metres): the point it looks past, above the body's feet; how far right
// of it and how far behind; how far it stays in front of what it meets, and the least it keeps
// from the point. It comes back out by this much each frame.
constexpr float kPivotHeight = 1.6f;
constexpr float kShoulder = 0.45f;
constexpr float kDistance = 2.4f;
constexpr float kWallGap = 0.25f;
constexpr float kNearest = 0.3f;
constexpr float kEaseOut = 0.06f;
// The third-person camera's orbit: floats from here (x, y, z, w), and half the turn it may reach
// either way from level (radians; 70 degrees).
constexpr std::size_t kCameraOrbitFloat = 4;
constexpr float kOrbitHalfLimit = 0.61f;
// The game's menus (C_UIApse: inventory, map, journal, ...) at services + 432, and their open state
// machine at + 104, whose vtable slot 1 reads the step: 0 or 1 while closed
// (character_creator/creator_stage.cpp).
constexpr std::size_t kServicesMenus = 432;
constexpr std::size_t kMenusState = 104;
constexpr std::size_t kStateStepSlot = 1;

ServicesGetter get_services = nullptr;
Logger log_line = nullptr;
Mode mode = Mode::Allowed;
// Whether the F5 key was down on the previous frame (its press is the edge).
bool key_was_down = false;
// The key's step through the views (view_steps.hpp): read by the view update for the distance.
std::atomic<int> view_step{0};
// Whether this module changed the view since the last reset.
bool changed_view = false;
// The third person was put aside for a game screen and comes back when it closes.
bool held_for_screen = false;
// The camera manager's view update, and how far behind the point the camera stood last frame.
using ViewUpdate = std::intptr_t(__fastcall*)(void* manager, float* params);
ViewUpdate original_view_update = nullptr;
float kept_distance = 0.0f;
// The camera's input listener and when it hears nothing.
using CameraInput = std::intptr_t(__fastcall*)(void* camera, const void* event);
CameraInput original_camera_input = nullptr;
MouseBlocked mouse_blocked = nullptr;
bool told_blocked = false;

const char* mode_name(Mode value) {
  switch (value) {
    case Mode::Disabled:
      return "disabled";
    case Mode::Forced:
      return "forced";
    case Mode::Allowed:
      break;
  }
  return "allowed";
}

std::uint8_t* camera_manager() {
  auto* services = get_services ? static_cast<std::uint8_t*>(get_services()) : nullptr;
  if (!services) {
    return nullptr;
  }
  return *reinterpret_cast<std::uint8_t**>(services + kServicesCameraManager);
}

// Whether one of the game's menus is open: they are panels in front of the first-person camera.
bool menus_open() {
  auto* services = get_services ? static_cast<std::uint8_t*>(get_services()) : nullptr;
  auto* menus = services ? *reinterpret_cast<std::uint8_t**>(services + kServicesMenus) : nullptr;
  if (!menus) {
    return false;
  }
  void* state = menus + kMenusState;
  using Step = std::uint8_t(__fastcall*)(void*);
  const std::uint8_t step = (*static_cast<Step**>(state))[kStateStepSlot](state);
  return step > 1;
}

void* local_body() {
  auto* services = get_services ? static_cast<std::uint8_t*>(get_services()) : nullptr;
  void* game = services ? *reinterpret_cast<void**>(services + kServicesGameInterface) : nullptr;
  if (!game) {
    return nullptr;
  }
  using LocalActor = void*(__fastcall*)(void*);
  void* actor = (*static_cast<LocalActor**>(game))[kLocalActorSlot](game);
  return actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + kActorEntity)
               : nullptr;
}

// What a client script asked for (Camera.setThirdPerson).
struct ScriptView {
  bool on = false;
  float distance = kDistance;
  float height = kPivotHeight;
  float shoulder = kShoulder;
};
std::mutex script_view_mutex;
ScriptView asked_view;
// Whether the script's view was shown last frame, so its end gives the first person back.
bool script_view_shown = false;

ScriptView script_view() {
  std::lock_guard<std::mutex> lock(script_view_mutex);
  return asked_view;
}

// v turned by the quaternion (x, y, z, w).
void turned(const float* rotation, const float v[3], float out[3]) {
  const float qx = rotation[0];
  const float qy = rotation[1];
  const float qz = rotation[2];
  const float qw = rotation[3];
  const float tx = 2.0f * (qy * v[2] - qz * v[1]);
  const float ty = 2.0f * (qz * v[0] - qx * v[2]);
  const float tz = 2.0f * (qx * v[1] - qy * v[0]);
  out[0] = v[0] + qw * tx + (qy * tz - qz * ty);
  out[1] = v[1] + qw * ty + (qz * tx - qx * tz);
  out[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

bool script_animation = false;

// Under a script animation the view's rotation follows the body's head, which a cutscene's
// clip bows and swings (the executioner's whipping put the camera over the head,
// looking straight down at it): the view keeps only its heading, kScriptPitch down. rotation: the
// view's quaternion x, y, z, w (the game's: forward is its y axis), set again; forward: its new
// forward.
constexpr float kScriptPitchRadians = -12.0f * 3.14159265f / 180.0f;

void level_view(float* rotation, float forward[3]) {
  const float flat = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
  if (!(flat > 1e-4f)) {
    return;
  }
  // The heading about z (forward (0, 1, 0) turned by yaw is (-sin, cos, 0)), then the pitch about
  // the view's own x.
  const float yaw = std::atan2(-forward[0] / flat, forward[1] / flat);
  const float sy = std::sin(yaw * 0.5f);
  const float cy = std::cos(yaw * 0.5f);
  const float sp = std::sin(kScriptPitchRadians * 0.5f);
  const float cp = std::cos(kScriptPitchRadians * 0.5f);
  // (0, 0, sy, cy) · (sp, 0, 0, cp)
  rotation[0] = cy * sp;
  rotation[1] = sy * sp;
  rotation[2] = sy * cp;
  rotation[3] = cy * cp;
  const float ahead[3] = {0.0f, 1.0f, 0.0f};
  turned(rotation, ahead, forward);
}

// Places the camera over the shoulder and in front of the world, with the game's rotation.
void place_camera(float* params) {
  void* body = local_body();
  if (!body) {
    return;
  }
  float feet[3] = {};
  kcds::runtime::props::entity::world_position(body, feet);
  const ScriptView seat = script_view();
  const float pivot_height = seat.on ? seat.height : kPivotHeight;
  const float shoulder = seat.on ? seat.shoulder : kShoulder;
  const float distance = seat.on ? seat.distance : view_steps::distance(view_step.load());
  const float pivot[3] = {feet[0], feet[1], feet[2] + pivot_height};
  const float ahead[3] = {0.0f, 1.0f, 0.0f};
  const float side[3] = {1.0f, 0.0f, 0.0f};
  float forward[3] = {};
  float right[3] = {};
  turned(params + 3, ahead, forward);
  if (script_animation) {
    level_view(params + 3, forward);
  }
  turned(params + 3, side, right);
  float way[3] = {};
  for (int axis = 0; axis < 3; ++axis) {
    way[axis] = right[axis] * shoulder - forward[axis] * distance;
  }
  const float length = std::sqrt(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
  if (!(length > 0.0f) || !std::isfinite(length)) {
    return;
  }
  float reach[3] = {};
  for (int axis = 0; axis < 3; ++axis) {
    way[axis] /= length;
    reach[axis] = pivot[axis] + way[axis] * (length + kWallGap);
  }
  float allowed = length;
  float hit = 0.0f;
  if (kcds::runtime::physics::first_hit(pivot, reach, hit)) {
    allowed = (std::clamp)(hit - kWallGap, kNearest, length);
  }
  kept_distance = allowed < kept_distance ? allowed : (std::min)(allowed, kept_distance + kEaseOut);
  for (int axis = 0; axis < 3; ++axis) {
    params[axis] = pivot[axis] + way[axis] * kept_distance;
  }
}

// Looking straight down under a script animation, how far the first-person camera moves ahead of
// the head along the ground (metres): the game's own at 65 degrees down, 0.14 m (sin 65 = 0.91).
constexpr float kAheadOfBody = 0.16f;

// Moves the first-person camera ahead of the body, by how far it looks down.
void keep_ahead_of_body(float* params) {
  const float ahead[3] = {0.0f, 1.0f, 0.0f};
  float forward[3] = {};
  turned(params + 3, ahead, forward);
  const float flat = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
  if (forward[2] >= 0.0f || !(flat > 0.0f)) {
    return;
  }
  const float shift = kAheadOfBody * -forward[2];
  params[0] += forward[0] / flat * shift;
  params[1] += forward[1] / flat * shift;
}

std::intptr_t __fastcall hooked_view_update(void* manager, float* params) {
  const std::intptr_t result = original_view_update(manager, params);
  const auto* bytes = static_cast<const std::uint8_t*>(manager);
  if (params && bytes[kManagerThirdPerson] != 0 && bytes[kManagerOtherView] == 0) {
    place_camera(params);
  } else {
    kept_distance = 0.0f;
    if (params && script_animation && bytes[kManagerThirdPerson] == 0 &&
        bytes[kManagerOtherView] == 0) {
      keep_ahead_of_body(params);
    }
  }
  return result;
}

// The game's camera turns up and down about its x axis with no limit: past straight down the world
// was upside down, live. orbit: its quaternion x, y, z, w.
void limit_orbit(float* orbit) {
  const float half = std::atan2(orbit[0], orbit[3]);
  const float limited = std::clamp(half, -kOrbitHalfLimit, kOrbitHalfLimit);
  if (limited == half) {
    return;
  }
  orbit[0] = std::sin(limited);
  orbit[1] = 0.0f;
  orbit[2] = 0.0f;
  orbit[3] = std::cos(limited);
}

std::intptr_t __fastcall hooked_camera_input(void* camera, const void* event) {
  const bool blocked = mouse_blocked && mouse_blocked();
  if (blocked != told_blocked && log_line) {
    log_line("CAMERA third person mouse %s", blocked ? "held for the UI" : "free");
  }
  told_blocked = blocked;
  if (blocked) {
    return 0;
  }
  const std::intptr_t result = original_camera_input(camera, event);
  limit_orbit(reinterpret_cast<float*>(camera) + kCameraOrbitFloat);
  return result;
}

// Selects the view and marks it changed for the game's post-update; writes nothing when it is
// already the wanted one.
void select_view(std::uint8_t* manager, bool third_person, const char* source) {
  std::uint8_t& view = manager[kManagerThirdPerson];
  const std::uint8_t wanted = third_person ? 1 : 0;
  if (view == wanted) {
    return;
  }
  view = wanted;
  manager[kManagerDirty] = 1;
  changed_view = true;
  if (log_line) {
    log_line("CAMERA third person %s (%s)", third_person ? "on" : "off", source);
  }
}

}  // namespace

bool install(ServicesGetter services, void* view_update, Logger logger) {
  get_services = services;
  log_line = logger;
  // Its key in the KCDS group of the game's key bindings, F5 by default.
  keybinds::add_builtin_group(keybinds::kKcdsGroup, keybinds::kKcdsGroupLabel);
  keybinds::add_builtin_action(kViewAction, keybinds::kKcdsGroup, "Third person view", "f5");
  const bool placed = view_update &&
                      MH_CreateHook(view_update, reinterpret_cast<void*>(hooked_view_update),
                                    reinterpret_cast<void**>(&original_view_update)) == MH_OK &&
                      MH_EnableHook(view_update) == MH_OK;
  if (log_line) {
    log_line("CAMERA third person placement %s", placed ? "installed" : "unavailable");
  }
  return get_services != nullptr;
}

bool install_mouse_gate(void* camera_input, MouseBlocked blocked) {
  mouse_blocked = blocked;
  const bool hooked = camera_input &&
                      MH_CreateHook(camera_input, reinterpret_cast<void*>(hooked_camera_input),
                                    reinterpret_cast<void**>(&original_camera_input)) == MH_OK &&
                      MH_EnableHook(camera_input) == MH_OK;
  if (log_line) {
    log_line("CAMERA third person mouse gate %s", hooked ? "installed" : "unavailable");
  }
  return hooked;
}

bool in_third_person() {
  const std::uint8_t* manager = camera_manager();
  return manager && manager[kManagerThirdPerson] != 0;
}

void test_mouse_event(float value) {
  std::uint8_t* manager = camera_manager();
  void* camera = manager ? *reinterpret_cast<void**>(manager + kManagerCamera) : nullptr;
  if (!camera || !original_camera_input) {
    return;
  }
  // SInputEvent as the listener reads it: device 1 (the mouse), state 8 (changed), key, value.
  constexpr std::size_t kEventBytes = 64;
  alignas(8) std::uint8_t event[kEventBytes] = {};
  const std::int32_t changed = 8;
  const std::int32_t orbit = 267;
  event[0] = 1;
  std::memcpy(event + 4, &changed, sizeof(changed));
  std::memcpy(event + 16, &orbit, sizeof(orbit));
  std::memcpy(event + 24, &value, sizeof(value));
  const auto* before = reinterpret_cast<const float*>(camera) + 4;
  const float was[4] = {before[0], before[1], before[2], before[3]};
  hooked_camera_input(camera, event);
  if (log_line) {
    log_line("CAMERA test mouse %.1f orbit (%.4f %.4f %.4f %.4f) -> (%.4f %.4f %.4f %.4f)", value,
             was[0], was[1], was[2], was[3], before[0], before[1], before[2], before[3]);
  }
}

void set_script_animation(bool running) {
  script_animation = running;
}

void set_script_view(bool on, float distance, float height, float shoulder) {
  std::lock_guard<std::mutex> lock(script_view_mutex);
  asked_view = {on, distance, height, shoulder};
}

void set_mode(Mode now) {
  if (mode == now) {
    return;
  }
  mode = now;
  if (log_line) {
    log_line("CAMERA third person mode %s by the server", mode_name(mode));
  }
}

void frame(bool game_window_foreground, bool text_input_open, bool game_screen_open) {
  const bool key_down = keybinds::held(kViewAction);
  const bool key_pressed = key_down && !key_was_down;
  key_was_down = key_down;
  auto* manager = camera_manager();
  if (!manager) {
    return;
  }
  if (game_screen_open || menus_open()) {
    if (manager[kManagerThirdPerson] != 0) {
      held_for_screen = true;
      select_view(manager, false, "game screen");
    }
    return;
  }
  if (held_for_screen) {
    held_for_screen = false;
    if (mode != Mode::Disabled) {
      select_view(manager, true, "game screen closed");
    }
  }
  const bool scripted = script_view().on;
  if (scripted && mode != Mode::Disabled) {
    script_view_shown = true;
    select_view(manager, true, "script");
    return;
  }
  if (script_view_shown) {
    script_view_shown = false;
    if (mode == Mode::Allowed) {
      select_view(manager, false, "script");
    }
  }
  const bool key_taken = key_pressed && game_window_foreground && !text_input_open;
  if (mode == Mode::Disabled) {
    view_step = 0;
    select_view(manager, false, "server");
  } else if (mode == Mode::Forced) {
    // The key steps through the distances only.
    if (key_taken) {
      view_step = view_steps::next(view_step.load(), false);
    }
    select_view(manager, true, "server");
  } else if (key_taken) {
    int step = view_step.load();
    if (manager[kManagerThirdPerson] == 0) {
      // In the first person, whatever step was kept.
      step = 0;
    } else if (step == 0) {
      // A third person the key did not bring stands at the usual step.
      step = view_steps::kUsualStep;
    }
    view_step = view_steps::next(step, true);
    select_view(manager, view_step.load() != 0, "key");
  }
}

bool script_view_on() {
  return script_view().on;
}

bool script_view_seat(float& distance, float& height) {
  const ScriptView seat = script_view();
  if (!seat.on) {
    return false;
  }
  distance = seat.distance;
  height = seat.height;
  return true;
}

void forget_script_view() {
  set_script_view(false, kDistance, kPivotHeight, kShoulder);
}

void reset() {
  mode = Mode::Allowed;
  script_view_shown = false;
  key_was_down = false;
  held_for_screen = false;
  if (!changed_view) {
    return;
  }
  changed_view = false;
  auto* manager = camera_manager();
  if (manager) {
    select_view(manager, false, "server");
  }
  changed_view = false;
}

}  // namespace kcds::runtime::camera::third_person
