// Free look (playAnimation lockRotation: true), WHGame 1.5.5: the player's head and
// camera turn, the body does not. A player tied to a stake looked around by turning the whole posed
// body away from it.
//
// While a request with lockRotation runs, the view is a camera of our own (anchor_view.hpp, as
// NoClip and the camera shots use) at the eyes: the body's Head joint (props::entity::joint_pose),
// kEyeForward in front of it along the view. The mouse turns it as NoClip's built-in driver reads
// it (the pointer kept in the window's centre), up to kYawLimit either side of where the body
// faces and kPitchLimit up and down. The game's player controls are held meanwhile (the action map
// "player" off, as NoClip and the game's menus do), so nothing turns or moves the body. Every other
// player sees the head turn: the look target the client publishes is the camera's line of sight
// (target()). When the request ends the game's own view comes back.
//
// Engine thread.
#pragma once

#include "anchor_view.hpp"

namespace kcds::runtime::camera::free_look {

using Logger = void (*)(const char* format, ...);

struct FreeLookNatives {
  Natives view;
  void* (*services)() = nullptr;  // the game services getter, for the action maps
  void* (*window)() = nullptr;    // the game window
  // Whether the mouse is the game's now (no chat, page or menu takes it).
  bool (*mouse_free)() = nullptr;
};

void install(const FreeLookNatives& natives, Logger logger);

// Every frame: free look while `wanted` (a request with lockRotation runs) and the body is in the
// world. hold_controls: the game's player controls are off meanwhile. A driver on a cart's seat
// keeps them to drive; the cart takes their turning away (carts/placed_carts.hpp). The view turns
// with the body when the body is turned, as it is by a moving cart. behind: the third person's view
// of a seated player, turning all the way round the body, not stopped by the cart, and kept above
// the ground. paused: another camera lies over this one (a scripted shot, NoClip) and comes back
// to it when it ends; nothing changes until then.
void keep(void* local_entity, bool wanted, bool in_world, bool hold_controls = true,
          bool behind = false, bool paused = false);

bool active();

// How far the view turns, in degrees: `yaw` to each side of where the body faces, `up` and `down`
// from level (playAnimation lockRotation { yaw, up, down }). A yaw of 0 brings back the
// defaults, kYawLimit and kPitchLimit. With limits of its own the view begins facing the body's
// heading, in the middle of its up and down range: a player sat at a board looks at it, as the
// game's dice players do. Called every frame before keep().
void set_limits(double yaw, double up, double down);

// One frame of the mouse, as the free look turns by it: the pointer's move from the window's
// centre since the last call, in degrees (to the left and up positive), the pointer put back in
// the centre. False (and 0) while the window is not in front or a chat, page or menu has the
// mouse. A camera shot with look (camera_shot.hpp) turns by it too. forget_mouse(): the next call
// starts afresh, so a stale place of the pointer does not jump the view.
bool mouse_turn(double& yaw_degrees, double& pitch_degrees);
void forget_mouse();

// Test hooks only: turns the view as this many degrees of mouse would, to the left.
void test_turn(double degrees);

// Where the camera looks, 10 m ahead of it, for the published look target; false while off.
bool target(float out[3]);

// The world went away: the camera is forgotten (its view went with it) and the controls come back.
void reset();

}  // namespace kcds::runtime::camera::free_look
