// The game's own third-person camera for the local player, on the F5 key and under the server's
// word (ThirdPerson.setMode, a KCDS addition), WHGame 1.5.5.
//
// The camera manager is the services object (get_services) + 56. Its byte +56 selects the view
// (0 first person, 1 the game's third-person camera) and its byte +58 says the view changed (the
// dirty flag). The game's own post-update (180 82EC48) then switches the head and the body, so a
// view change writes those two bytes and nothing else.
//
// A client script may ask for this view for its own machine (Camera.setThirdPerson),
// with where the camera sits. It holds the view until it is turned off; the server's disabled still
// wins.
//
// Modes: allowed (F5 steps from the first person through three third-person distances and back,
// view_steps.hpp; the first person until the key is pressed), disabled (first person only), forced
// (third person only, F5 steps through the distances). Out of a world the server's word is
// gone and the view goes back to first person.
//
// The game's own screens and minigames are made for the first person: in the third person the
// inventory screen opens without its panels and the alchemy camera turns out through the walls
// While one is open the view is held in the first person, and the third
// person comes back when it closes.
//
// Where the camera stands: the game's third-person camera (camera manager + 40) is a free one with
// no collision, turning about a point ahead of the body, so it went into walls, trees and under the
// ground. The manager's view update has it fill the view's parameters (SViewParams:
// position floats 0-2, rotation quaternion x, y, z, w floats 3-6); after it the camera is placed
// again with the game's rotation: over the right shoulder, behind a point at head height above the
// body, and in front of the first solid thing between that point and there (the line-of-sight
// ray, physics/line_of_sight.hpp: terrain, static geometry, vegetation, props). It comes forward
// at once and goes back out gently.
//
// The first-person camera under a script animation: looking down, the game keeps its camera ahead
// of the body (0.14 m ahead of the head, 0.2 m of the spine, measured at 65 degrees down), so the
// body stays under and behind the view. While Player.playAnimation runs, the camera sits on the
// head joint, right above the chest: looking down went through the inside of the torso, which the
// renderer does not draw from there (a hollow body, the trousers from above, a strip of skin across
// the screen where the near plane cut the chest). The same view update then moves it ahead again.
//
// The mouse: this camera listens to the engine's input events itself (slot 2: mouse key
// 267 orbits it, 268 changes its distance), not to the action maps, so turning the action maps off
// for a focused page did not stop it. The listener is hooked and those events are
// dropped while the UI owns the mouse.
//
// Engine thread.
#pragma once

#include <cstdint>

namespace kcds::runtime::camera::third_person {

using Logger = void (*)(const char* format, ...);
using ServicesGetter = void* (*)();

// What the server allows. The values are the wire's (kThirdPerson* in third_person.hpp of the
// integration library).
enum class Mode : std::uint8_t {
  Allowed = 0,
  Disabled = 1,
  Forced = 2,
};

// services: the game's services getter; view_update: the camera manager's view update
// hooked to keep the camera out of the world (null leaves the game's own camera).
// False when the services getter is missing.
bool install(ServicesGetter services, void* view_update, Logger logger);

// camera_input: the third-person camera's input listener; blocked says when it hears
// nothing. False when it could not be hooked: the camera then turns with the mouse under a page.
using MouseBlocked = bool (*)();
bool install_mouse_gate(void* camera_input, MouseBlocked blocked);

// Test hooks only: a mouse event of this size straight to the camera's listener, through the gate.
// The camera's orbit is logged before and after.
void test_mouse_event(float value);

// Camera.setThirdPerson: on or off, and where the camera sits (metres). Any thread.
void set_script_view(bool on, float distance, float height, float shoulder);

// The server's mode for the local player (allowed until it says otherwise).
void set_mode(Mode mode);

// Whether the view is the third person now.
bool in_third_person();

// Whether a script animation runs under the player's own camera (see above).
void set_script_animation(bool running);

// Engine thread, every frame in a world. game_window_foreground: the game's window has the focus;
// text_input_open: the chat, a focused page or a menu owns the keyboard; game_screen_open: one of
// the game's own screens (inventory, map, ...) or crafting minigames is open, and the first person
// is held until it closes. In the mode allowed the F5 key toggles the view (on its press, only
// with the focus, no text input and no game screen); in the modes disabled and forced the view is
// held.
void frame(bool game_window_foreground, bool text_input_open, bool game_screen_open);

// Whether a client script holds the third person (Camera.setThirdPerson). Any thread.
bool script_view_on();

// While a client script holds the third person: how far behind and how high above the feet it
// asked the camera to look from (metres); false while none does. Any thread.
bool script_view_seat(float& distance, float& height);

// Out of a world or at the end of a session: the mode allowed again and, when the camera was
// changed, back to first person. A client script's view stays: the resources run on into the
// next world (a client.js that asked for it on load ran before the server's world was loaded,
// and its view was lost).
void reset();

// The session ended (the client resources stopped): a script's view goes too. Any thread.
void forget_script_view();

}  // namespace kcds::runtime::camera::third_person
