// ActionHint for client resources: the game side of
// Client/resources/action_hints.cjs.
//
// A hint is drawn by the game's own HUD hint list. The game's hint manager hands the list of
// active hints (S_ActionHint, WHGame 1.5.5) to the HUD's listener, C_UIHudHints,
// whenever it changes; the hook appends one S_ActionHint per resource hint, built with the
// game's own constructor on the control's action and action map, so the HUD draws
// the glyph of whatever the player bound to that control. A press of that action, or holding it
// for the hint's hold duration, is reported back to the runtime.
//
//   runtime -> game:  { op: "actionHints", hints: [...] }, each
//                     { id, key, text, mode, holdDuration, enabled }
//   game -> runtime:  { op: "actionHintFired", id }
//                     { op: "actionHintsShown", hints: [...] }, each
//                     { key, map, text, mode, enabled, progress, target }: every hint the HUD
//                     draws, the game's own and interaction prompts too, sent when the set or a
//                     hold ring changes (at most every kShownIntervalMs; ActionHint.shown, issues)
#pragma once

#include <functional>
#include <string>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::action_hints {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
using Logger = void (*)(const char* format, ...);
using StringAssign = void*(__fastcall*)(void* string, const char* text);

struct Natives {
  void* hud_hints_listener = nullptr;  // C_UIHudHints hint-list listener
  void* hint_constructor = nullptr;    // S_ActionHint::S_ActionHint
  StringAssign string_assign = nullptr;
  // True while a game menu (map, inventory, ...) is up: hints are drawn only in free play.
  bool (*menu_open)() = nullptr;
};

// Hooks the listener; false when a native is missing or the hook fails.
bool install(const Natives& natives, Logger logger);

// For local::Client::set_local_runtime_handler: true when the message was ActionHint's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// From the player's input action hook (engine thread): the action's name and activation mode
// (1 press, 2 release). True when an enabled hint took the press: the game's own action on that
// key does not run.
bool on_player_action(const char* action, unsigned mode);

// A hint the client's own features show (engine thread), beside the resources' hints: id a
// negative number of the feature's, key one of the resource hint keys, text a localization key
// when localized (the game's own wording), on_fire called on the engine thread when it fires.
struct EngineHint {
  std::int64_t id = 0;
  std::string key;
  std::string text;
  bool localized = false;
  bool hold = false;
  double hold_seconds = 1;
  bool enabled = true;
  std::function<void()> on_fire;
};
void set_engine_hint(const EngineHint& hint);
void clear_engine_hint(std::int64_t id);

// Engine thread: hides the pause screen's own hints (its page tabs, the hints in its corners and
// at its foot) or draws them again; the CharacterCreator hides them while its stage is up.
void hide_menu_hints(bool hidden);

// Engine thread: hides the game's own hints in the world (Loot, Pick up body, ...) or draws them
// again, while the player lies knocked out and cannot act on them. A script's hints
// and the pause screen's stay.
void hide_world_hints(bool hidden);

// Engine thread: hides the game's own hint on this action (its S_ActionHint's action) or draws
// it again; the Follow of a script that wants no "Stop following" prompt hides it.
void hide_game_hint(const char* action, bool hidden);

// Engine thread, every frame: fills hold rings and redraws the hint list when it changed.
// keys_open: the game window is in front and nothing captures typed input (the script action key
// is read from the keyboard then).
void frame(bool in_world, bool keys_open);

// Network thread: reports fired hints to the runtime of session; a new session drops all hints.
void pump(const void* session, const RuntimeSender& send);

}  // namespace kcds::runtime::action_hints
