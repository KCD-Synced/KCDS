// The game's lamp hold on the local player's body while they carry a lantern in the left hand
// (Player.setLantern, Backend/scripting/runtimes/js/lanterns/lanterns.cjs). The game's NPCs hold
// their lamp tool (an item of manipulation type 27, tag "lamp") with the fragment HoldItemLeft
// and the tag l_lamp: the arm bent, the lamp hanging from its handle in the hand's item slot. It
// plays in the hold scope only, so the legs walk and run under it, and the game picks its idle,
// walk and run clips (holditem_*_top_lhand_over; the women's database has its own walk and run).
// The other clients see it as they see the body's other animations, from this client's capture.
// A rider carries the lantern in the right hand: no hold then.
//
// The server's reserved client event kcds:lantern { pose } reaches the client worker, which
// sends { op: 'lanternPose', pose }.
//
// The game's torch is drawn into the left hand too, with its own hold. Whether the left hand
// holds a torch goes to the worker as { op: 'lanternTorch', torch } each time it changes, and on
// to the server (kcds:lantern { op: 'torch', drawn }), which moves the lantern to the right hand
// while the torch is out. The lamp hold ends at once when a torch is drawn.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::lanterns::pose {

// { op: 'lanternPose', pose: bool }; false for another message. Any thread.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Holds the lamp while the server asks for it and the player is not riding (engine thread,
// every frame in a world).
void frame(void* local_actor, bool riding);

// Sends a change of the torch in the left hand to the worker (the session's pump).
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
void pump(const RuntimeSender& send);

}  // namespace kcds::runtime::lanterns::pose
