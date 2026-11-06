// The game's own HUD for client resources: the game side of
// Client/resources/hud.cjs.
//
// The HUD is the game's FlashUI element "hud" (Libs/UI/UIElements/HUD.xml in the game's data).
// Everything runs through the game's natives on the engine thread (no script):
//
//   - its message methods are called on the element (NativeUI's elementCall, IUIElement
//     CallFunction) the way the game's UIAction.CallFunction does;
//   - tutorials by id go to the game's tutorial manager (game services + 296, then + 32), as the
//     Game script binding does: Show, Reset (ShowTutorial resets first),
//     Hide, and the tutorial display (services + 224, then + 376) slot 18 hides the
//     current one;
//   - HUD elements (the movie's clips) a resource hides are hidden again before every frame
//     (their display info, as UIAction.SetVisible does), because the game sets their visibility
//     itself, and given back their visibility on release;
//   - screen effects are the 3D engine's post-effect parameters (I3DEngine slot 171,
//     SetPostEffectParam, what System.SetScreenFx calls; the engine).
//
//   runtime -> game:  { op: "hud", call, args: [...] }
//                     { op: "hudHidden", elements: [...] }  every element some resource hides
//                     { op: "screenFx", values: { <engine post-effect parameter>: number } }
//                     { op: "cameraWanted" }  a resource projects world points (worldToScreen)
//   game -> runtime:  { op: "hudHost", ready, dialogue }: whether the HUD exists (in a world,
//                     not loading) and whether the local player is in one of the game's
//                     conversations
//                     { op: "camera", m: [12], fov } the view camera whenever it changes, once
//                     wanted, and { op: "camera" } without one outside a world
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::hud {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
using StringAssign = void*(__fastcall*)(void*, const char*);

struct Natives {
  void* (*services)() = nullptr;
  void* string_init = nullptr;  // the game's string: empty
  void* string_free = nullptr;  // and released
  StringAssign string_assign = nullptr;
  void* tutorial_show = nullptr;
  void* tutorial_reset = nullptr;
  void* tutorial_hide = nullptr;
  void* const* engine_3d = nullptr;
  // Lets the game show tutorials while the resources' tutorial calls run (the client keeps the
  // game's own tutorials off in a multiplayer world).
  void (*allow_tutorials)(bool allow) = nullptr;
};

// False when a native is missing.
bool install(const Natives& natives);

// For local::Client::set_local_runtime_handler: true when the message was a HUD call.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Reports the HUD and the game's dialogue state to the runtime of session when they change.
void pump(const void* session, const RuntimeSender& send, bool hud_ready, bool in_dialogue);

// The server shows or hides the game's combat marker for this session (any thread).
void set_combat_marker_shown(bool shown);

// The server shows or hides the game's restricted-area marker for this session (any
// thread).
void set_restricted_area_marker_shown(bool shown);

// The server allows this player in private areas (any thread): the same marker stays
// hidden, whatever set_restricted_area_marker_shown says.
void set_trespass_allowed(bool allowed);

// The local player lies knocked out, or is up again (any thread): the health and
// stamina bar is hidden meanwhile.
void set_knocked_out(bool down);

// Engine thread: the view camera of this frame (CCamera matrix rows and vertical field of view),
// or none outside a world.
void set_camera(const float* matrix, float fov);
void clear_camera();

// Engine thread, every frame: runs the Hud calls queued since the last frame, in order, keeps
// the hidden elements hidden and gives the released ones back, and sets the screen-effect
// parameters that changed. Outside a world the calls are dropped and the screen effects wait.
void frame(bool in_world);

}  // namespace kcds::runtime::hud
