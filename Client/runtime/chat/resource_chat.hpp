// The Chat global and the reserved chat events of client resources (issue):
// the game side of Client/resources/chat.cjs.
//
// Lines typed in the native chat overlay go to the resource runtime first, which runs the
// chatSend handlers and sends back the lines that may leave; every line the server sends this
// player is raised there as chatMessage. Resources also show, hide, open and close the overlay.
//
//   game -> runtime:  { op: "chatTyped", text }
//                     { op: "chatMessage", author, text, color }
//                     { op: "chatHost", open, visible }  when the overlay's state changes
//   runtime -> game:  { op: "chatOut", text }  a line to send to the server
//                     { op: "chatUI", visible }, { op: "chatOpen" }, { op: "chatClose" }
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "kcd2s/integration/chat.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::chat {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

// For local::Client::set_local_runtime_handler: true when the message was a chat call.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// A typed line for the chatSend handlers; false when the runtime did not take it.
bool forward_typed(const RuntimeSender& send, const std::string& text);

// A line the server sent this player, raised as chatMessage.
void forward_received(const RuntimeSender& send, const kcd2s::integration::ChatMessage& message);

// The lines the resources let leave or sent with Chat.send, in order.
std::vector<std::string> take_outgoing();

// Reports the overlay's state to the runtime of session; a new session shows the overlay again.
void pump(const void* session, const RuntimeSender& send);

}  // namespace kcds::runtime::chat
