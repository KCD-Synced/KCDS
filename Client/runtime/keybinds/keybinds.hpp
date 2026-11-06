// Client Keybinds: resource actions in the game's own key binding settings, the
// player's choices kept on this computer for each server, and the bound keys told to the resource
// runtime (Client/resources/keybinds.cjs).
//
//   runtime -> game:  { op: "keybinds", groups: [{ name, label }],
//                       actions: [{ name, group, label, key, controller }] } the whole set;
//                     { op: "keybindSet", name, keys?: [inputs], controller?: input };
//                     { op: "keybindReset", name }
//   game -> runtime:  { op: "keybindChoices", actions: [{ name, keys: [inputs], controller }] }
//
// Inputs are the engine's names ("f6", "np_1", "mouse4", "xi_a"). Names are kcds_ followed by
// lower-case letters, digits and underscores.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::keybinds {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

// The server joined ("host:port", or its listing id): its saved choices are read. Any thread.
void set_server(const std::string& server);

// For local::Client::set_local_runtime_handler: true when the message was a key binding one.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Reports the bound keys to the resource runtime of session; network thread, after each tick.
void pump(const void* session, const RuntimeSender& send);

// An action of the game client itself, shown with the resources' actions. Any thread.
void add_builtin_group(const std::string& name, const std::string& label);
void add_builtin_action(const std::string& name, const std::string& group, const std::string& label,
                        const std::string& keyboard);
// The inputs an action is bound to: the player's choice, else its default. Any thread.
std::vector<std::string> keys_of(const std::string& name);

// Engine thread, every frame: shows the actions on the game's screen, reads the player's choices.
void frame();

}  // namespace kcds::runtime::keybinds
