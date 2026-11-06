// A client script's view shake (Camera.shake): the game's own shake of the
// active view, as its Camera:ViewShake flow node starts it, so a kick or a blow tilts
// the first-person view and lets it settle back. The view system (gFramework + 232, IViewSystem
// slot 7 GetActiveView) gives the view, whose slot 8 is IView::SetViewShake(&shakeAngle radians,
// &shakeShift metres, duration, frequency, randomness, shakeID, flip, updateOnly, groundOnly).
//
// handle_runtime_message on any thread, tick on the engine thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::camera::shake {

// A cameraShakeLocal message, queued for the engine thread: true when the message was one.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Starts the queued shakes on the active view of `framework` (IGameFramework); nothing without a
// world or a view.
void tick(void* framework, bool in_world);

}  // namespace kcds::runtime::camera::shake
