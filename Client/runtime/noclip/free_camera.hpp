// NoClip's free camera: the game side of
// Client/resources/noclip.cjs.
//
// The camera is an anchor view (camera/anchor_view.hpp): a view of the engine's own view system
// following a bodyless anchor entity, given its pose every frame; the player's own view comes back
// when the flight ends.
//
// In body mode the player's entity flies with the camera: its physics is switched off (the
// physics component, got, slot 24 EnablePhysics, as the script EnablePhysics
// does) and it is placed under the camera every frame (SetWorldTM), so it passes through
// geometry and the other players see it fly; when the flight ends its physics comes back where
// the camera stopped, or where it took off from.
//
// While a flight runs the game's player controls are held (the action map "player" disabled,
// as the game's menus do) and, with input on, the keys of the game's photo mode by their place on
// the keyboard (W A S D, Space up, C down, Shift boost, Alt crawl) and the mouse fly the camera.
//
//   runtime -> game:  { op: "noclipEnable", mode, input, speed, fov } (fov 0 keeps the game's)
//                     { op: "noclipDisable", keepPosition }
//                     { op: "noclipPose", x, y, z, yaw, pitch }
//                     { op: "noclipSettings", input, speed, fov }
//                     { op: "noclipMove", forward, strafe, lift, yaw, pitch, boost, crawl, dt }
//   game -> runtime:  { op: "noclipState", active, input, x, y, z, yaw, pitch, speed, fov,
//                     renderedFov, aspect } when it changes, { op: "noclipEnded", reason }
//                     when the game ended or refused a flight (the world went away, or reason
//                     "notAllowed"), and { op: "noclipAllowed", allowed } when the server
//                     changed it and to a new runtime session
//
// The server may forbid the local player NoClip (Player.noclipAllowed): while it is
// forbidden noclipEnable is refused and a flight that runs ends.
//
// Yaw is in degrees from +Y, rising to the left; pitch is in degrees, positive up.
//
// Engine thread for frame, network thread for the rest.
#pragma once

#include <functional>

#include "../camera/anchor_view.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::noclip {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
using Logger = void (*)(const char* format, ...);

struct Natives {
  void* (*framework)() = nullptr;  // gFramework (IGameFramework)
  void* (*entity_system)() = nullptr;
  void* spawn_params_init = nullptr;  // SEntitySpawnParams::SEntitySpawnParams
  void* physics_component = nullptr;  // the entity's physics component
  void* (*local_entity)() = nullptr;  // the local player's entity
  void* (*services)() = nullptr;      // the game services getter
  void* (*window)() = nullptr;        // the game window
};

bool install(const Natives& natives, Logger logger);

// For local::Client::set_local_runtime_handler: true when the message was NoClip's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Engine thread, every frame: flies and places the camera.
void frame(bool in_world, double seconds);

// Engine thread: whether the free camera flies (the view is not the player's eyes).
bool active();

// Engine thread: whether the free camera turns with the mouse now (a flight with its own input
// and mouse look always on): the pointer is put back in the window's middle every frame then, so
// it is of no use to a page and is not shown.
bool steers_pointer();

// Engine thread: the flying camera's pose, its vertical field of view (degrees) and the window's
// width over height, as noclipState reports them; false when it does not fly.
bool camera_view(camera::Pose& pose, double& fov_degrees, double& aspect);

// Engine thread: whether a flight hides the player's own body from them (every flight but one
// started with showBody).
bool hides_body();

// Engine thread, once the engine adapter stopped (an engine exception): ends a flight where it is,
// so the player gets its own view, controls and body back instead of a camera frozen in the air.
void abandon();

// Network thread: sends the camera's state to the runtime of session.
void pump(const void* session, const RuntimeSender& send);

// Whether the server lets the local player fly (Player.noclipAllowed); allowed again
// whenever the player leaves a world. Any thread.
void set_allowed(bool allowed);

}  // namespace kcds::runtime::noclip
