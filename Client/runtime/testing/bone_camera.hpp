// Test hook (KCDS_TEST_HOOKS only; Local\KCD2S.BoneCamera.<pid>): this client's camera follows a
// joint of a named body, close, so a kit can film what a hand holds or how a foot steps.
// %TEMP%\kcds-bonecam-<pid>.txt holds "<entity name> <joint> <x> <y> <z> <fov>" (the camera that
// far from the joint in the world, looking at it) or "off"; each signal reads it again.
//
// Engine thread.
#pragma once

#include <optional>

#include "kcd2s/integration/camera_shot.hpp"

namespace kcds::game_client {

// Creates the hook's event; called by install_test_hooks with KCDS_TEST_HOOKS.
void install_bone_camera();

// This frame's camera order of the hook: none while it is off and was not just turned off, an
// empty order (clear) once when it is turned off, else a shot at the joint.
std::optional<std::optional<kcd2s::integration::CameraShot>> bone_camera_order();

}  // namespace kcds::game_client
