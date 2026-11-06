// The server's scripted camera shot for the local player (Camera.startShot / endShot).
// A shot is a view of the engine's view system of our own (anchor_view.hpp, the
// camera NoClip and the CharacterCreator use): the game's camera keeps running underneath and
// comes back when the shot is cleared. A shot moves from where the camera is (the game's camera,
// or the shot before) to its pose over its seconds, eased at both ends; its field of view goes
// with it (0 keeps the game's own). A rotation is read as heading, elevation and roll; a look-at
// shot takes its roll from the shot. Heading and roll turn the short way round.
//
// A shot ends without the server clearing it when the world changes (the view system is rebuilt
// under a loading screen), when the view cannot be made or when it was taken away; the server is
// told (CAMERA_SHOT_ENDED).
//
// A client script may film this machine's view itself (Camera.startShot on the client,
// Client/resources/camera.cjs): its orders (cameraShotLocal, cameraClearLocal) are played the
// same way, the newest order of either side wins, and a script's shot that ended by itself goes
// back to the scripts (cameraShotLocalEnded), not to the server.
//
// Engine thread, except handle_runtime_message and pump.
#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "kcd2s/sdk/value.hpp"

#include "anchor_view.hpp"
#include "kcd2s/integration/camera_shot.hpp"

namespace kcds::runtime::camera {

struct ShotNatives {
  Natives view;
  // Tells the server a shot ended by itself; false when it could not go.
  bool (*send_ended)(kcd2s::integration::CameraShotEnd reason) = nullptr;
};

void install_shots(const ShotNatives& natives);

// Applies the server's orders (a shot, or empty for a clear) in order and moves the camera.
void tick_shots(bool in_world, double seconds,
                const std::vector<std::optional<kcd2s::integration::CameraShot>>& orders);

bool shot_active();

// A client script's shot or clear (any thread): true when the message was one.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Tells the scripts a shot of theirs ended by itself (network thread).
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
void pump(const RuntimeSender& send);

}  // namespace kcds::runtime::camera
