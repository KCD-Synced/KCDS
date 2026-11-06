// The game's own tutorial hint with a resource's content: the game
// side of the Hud tutorial hint calls in Client/resources/hud.cjs.
//
// The hint box in the top-left corner is the HUD movie's tutorial (HUD.xml ShowTutorial: id,
// text with <head>/<body> and $action; key tokens the movie draws as key glyphs, duration,
// priority, layout, the "close tutorial" key hint). The runtime shows and hides its hints through
// the HUD element like any NativeUI call. The game drives the same box through the element's
// call-by-name (IUIElement vtable + 552, the game's helper and the like); a hook on it
// reports the game's tutorial calls on the HUD, so the runtime knows when the game took a hint
// down (the player opened a menu, the game showed its own tutorial):
//
//   game -> runtime:  { op: "tutorialHintGame", call, id } for the HUD's ShowTutorial,
//                     HideTutorial, HideCurrentTutorial and HideAllTutorials called by the game,
//                     "TutorialsStopped" when the game stops the tutorial events
//                     (SetPlayingTutorialEvent(false): the pause screen or a menu came up, the
//                     box is gone) and "HudHidden" when the HUD element is hidden
//
// Engine thread for frame, network thread for pump.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::hud::tutorial_hint {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
using Logger = void (*)(const char* format, ...);

void install(Logger logger);

// Engine thread, every frame: hooks the HUD's call-by-name once the HUD exists.
void frame(bool in_world);

// Network thread: sends the game's tutorial calls to the runtime of session.
void pump(const void* session, const RuntimeSender& send);

}  // namespace kcds::runtime::hud::tutorial_hint
