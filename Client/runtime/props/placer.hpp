// The local player's prop placement (PLACER and PLACER_REPORT in
// kcd2s/integration/previews.hpp): a preview (ghosts::Preview) that follows the crosshair.
//
// Each frame a ray goes from the view camera along its view for the session's range
// (physics::first_hit, the game's RayWorldIntersection); where it meets the world, the preview
// stands on the hit (onSurface), turned onto the surface's normal with alignToSurface. When it
// meets nothing, the preview stands fallbackDistance along the view, dropped onto the ground
// below when there is ground within 50 m. verticalOffset lifts it. Its turn about the vertical
// is the server's yaw (degrees) plus what the player turned it, and with faceAwayFromPlayer the
// camera's heading as well.
//
// A spot is valid unless the server vetoed the session, the rules refuse it (requireSurface off a
// surface, a slope over maxSlope, nearer than minDistance or farther than maxDistance from the
// player) or it is not finite; the preview is tinted with validTint or invalidTint. The surface
// and clearance rules are the server's alone.
//
// The keys while placing: the left mouse button confirms a valid spot, the right one cancels, Q
// and E turn the preview (90 degrees a second). The game gets none of the player's actions but
// walking, looking around and the menu (refuse_action), so the buttons do not attack or block.
// Keys count only while the game window has the focus and no chat, view or screen takes the
// input; a key held when the session began counts once it was let go.
//
// Reports: `confirm` with the pose, then `end` unless keepOpen; `cancel` then `end`; with
// reportMoves a `move` at most ten times a second, when the pose or its validity changed. An end
// the server ordered (placerEnd) is not reported back.
//
// Engine thread.
#pragma once

#include <vector>

#include "kcd2s/integration/previews.hpp"

namespace kcds::runtime::props::placer {

using Logger = void (*)(const char* format, ...);

struct Frame {
  const float* camera = nullptr;  // the view camera's 3x4 world matrix, null when unknown
  const float* player = nullptr;  // the local player's position, null when unknown
  bool keys = false;              // the keys count this frame (focus, no UI over the game)
  double now = 0;                 // seconds
};

void install(Logger logger);

// Begins, vetoes or ends the session.
void apply(const kcd2s::integration::PlacerOrder& order);

// Moves the preview and handles the keys; the reports of this frame go to `reports`.
void frame(const Frame& frame, std::vector<kcd2s::integration::PlacerReport>& reports);

// True while a session runs.
bool active();

// Turns the preview by `degrees` (positive anticlockwise seen from above), as Q and E do; false
// without a session.
bool rotate(float degrees);

// True when the game must not get this action of the local player while placing (`mode` 1
// pressed, 2 released, 4 held, as the action handler gets it).
bool refuse_action(const char* action, unsigned mode);

// Forgets the session and its preview, as the game's level does when it unloads (the server sends
// a running session again when the world is ready).
void reset();

}  // namespace kcds::runtime::props::placer
