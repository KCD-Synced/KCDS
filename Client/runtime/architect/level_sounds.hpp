// World Architect's sounds (issue list C5): the game's own world sounds, an ambient loop (a
// village, a pond, bees, a fire, a crypt) placed at a point and heard there until taken away, in
// this game only. Local mode only; nothing is replicated.
//
// The sounds are the triggers of the game's world audio controls (Libs/GameAudio/world.xml,
// <ATLTrigger atl_name="..." path="...">): its loops and ambiences. A placed one is a free audio
// proxy kept playing at its point (audio/resource_audio.hpp start_loop).
//
//   { op: 'localSoundStart', id, trigger, at }  plays trigger at at {x, y, z}, as the editor's id
//   { op: 'localSoundMove', id, at }            moves it
//   { op: 'localSoundStop', id }                stops it
//   sync localSoundList                         [{ name, group }] the world's loops and ambiences
//
// Requests: any thread (handle_runtime_message). frame(): engine thread. pump(): network thread.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::level_sounds {

// True when the message was one of the above.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Starts, moves and stops what was asked and answers the list, in a ready world.
void frame(bool in_world);

// The world is left: every placed sound is stopped.
void reset();

// Sends the answers made.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::level_sounds
