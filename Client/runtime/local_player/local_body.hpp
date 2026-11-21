// The local player as this game has it now, for the client scripts' LocalPlayer: where the body
// is and how it is turned, its velocity, where the eyes look (the field "gaze") and its vitals.
// LocalPlayer used to read them from the server's directory, which echoes the player's own values
// back about a round trip and up to two 100 ms publish intervals late. The engine thread records
// them every frame; once a client script reads the local player (the worker's
// { op: 'localBodyWanted' }), the network thread sends them to the worker whenever they changed
// by more than a reading shows:
//
//   game -> runtime: { op: 'localBody', p: [x, y, z], q: [w, x, y, z], v: [x, y, z],
//                      g: [x, y, z], vitals: [9 numbers, kVitalNames order] | null }
//                    { op: 'localBody' } without them outside a world.
#pragma once

#include <array>
#include <functional>
#include <optional>

#include "kcd2s/core/math.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::local_body {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

struct Body {
  kcd2s::core::Vec3 position{};
  kcd2s::core::Quat rotation{};
  kcd2s::core::Vec3 velocity{};
  kcd2s::core::Vec3 gaze{};
  std::optional<std::array<float, 9>> vitals;
};

// Engine thread, every frame in a world: the local body now.
void set(const Body& body);

// The last body set (any thread), or none before the first.
std::optional<Body> latest();

// The worker's { op: 'localBodyWanted' }: true when it was this module's message.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Network thread: sends the body when a worker wants it and it changed; outside a world (the main
// menu, a loading screen) it says there is none. `session` identifies the runtime: a new one has
// to ask again.
void pump(const void* session, const RuntimeSender& send, bool in_world);

// Whether two readings differ by more than the message shows (a millimetre, a thousandth of a
// turn or a unit vector, a hundredth of a vital).
bool differs(const std::optional<Body>& a, const std::optional<Body>& b);

// The worker's message for a body, or for none.
kcd2s::sdk::Value message_of(const std::optional<Body>& body);

}  // namespace kcds::runtime::local_body
