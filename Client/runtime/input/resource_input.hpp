// Key bindings and Controls of client resources: the game side of
// Client/resources/input.cjs.
//
//   runtime -> game:  { op: "keyTable", keys: [{ name, kind: "scan"|"vk"|"pad", code }] } once a
//                     resource uses Key or Keybinds (pad: an XInput button bit, 0x10000 the left
//                     trigger, 0x20000 the right); { op: "controls", disabled,
//                     cursor } when the resources'
//                     Controls holds change.
//   game -> runtime:  { op: "keyEdges", edges: [[name, "down"|"up"]], down: [names] } while binds
//                     may fire; { op: "keyLabels", labels } for the player's layout;
//                     { op: "inputHost", captured, pointer } when typed input is taken.
#pragma once

#include <functional>
#include <string>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::input {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

// For local::Client::set_local_runtime_handler: true when the message was a key or controls one.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// What decides whether binds fire, sampled by the caller.
struct Gate {
  void* window = nullptr;  // the game window (HWND)
  bool in_world = false;   // the client is in a session with its player spawned
  bool captured = false;   // the chat, a focused view or an overlay takes typed input
  bool pointer = false;    // the pointer is shown for one of them
};

// Polls the keys and reports to the resource runtime of session (see resource_web.hpp). Call after
// every client tick, on the thread that ticks the client.
void pump(const void* session, const RuntimeSender& send, const Gate& gate);

// The resources' Controls holds: gameplay input taken, pointer drawn.
bool controls_disabled();
bool controls_cursor();

// Whether a resource holds this game action off (Controls.blockAction). Any thread.
bool action_disabled(const char* action);

// Whether one input event of a game action is held off (mode 1 press, 2 release, others held):
// a press while the action is blocked, and the release of a press that was held off. The game
// acts on some releases (the seats' sitting_exit_up and the others got the player up on the
// release of W, live), and a release of a press the game never saw is not one it needs. A
// press that goes through forgets that. A release of a press made before the block goes through,
// so no key stays down. Any thread.
bool hold_off(const char* action, unsigned mode);

// Whether the game's seat action map ("sitting_chair") is to be held off: a resource blocks one
// of the seat's exits (sitting_exit_up, _down, _left or _right). The seat registers them as
// callbacks of the action map that it calls itself, past the player input, so a
// blocked W or arrow still got the player up; off, the map sends nothing. Any thread.
bool sitting_map_held();

}  // namespace kcds::runtime::input
