// The client resources' Audio global: the game's own sound triggers
// played, stopped and steered on this machine, WHGame 1.5.5. Nothing is replicated.
//
// Names resolve through the audio system (IAudioSystem, crafting/sounds.hpp): slot 7 a trigger,
// 8 a continuous parameter (RTPC), 9 a switch, 10 one of its states (switch, name, out), each
// true when the name is declared. An entity's audio component (sounds.hpp audio_of, made when
// missing) takes them: slot 30 ExecuteTrigger and 31 StopTrigger (sounds.hpp play), 32 its every
// trigger stopped (aux object 0: all of them), 33 SetSwitchState(switch, state, aux object) and
// 34 SetRtpcValue(rtpc, value, aux object), as the entity binding's StopAudioTrigger,
// SetAudioSwitchState and SetAudioRtpcValue call them.
//
// A file (Audio.playFile) plays as the game plays a spoken line or a cutscene's sound
// the switch standalone_file_purpose set to the purpose's state (inner, 10 ... 200,
// sfp_dialog, sfp_cutscene, sfp_music: the voice host events event:/voice/dialog/*) with slot 33
// on aux object 1, then slot 28 PlayFile(file without extension, start 0, aux object 1, callback
// infos). The game's audio allows one file per object each wh_snd_playfile_cooldown (0.1 s).
//
// A sound at a point is a free audio proxy (the audio system's slot 18, CAudioProxy): slot 1
// Initialize(name, asynchronous), 13 SetPosition(3 x 4 matrix), 6 ExecuteTrigger(trigger, lip
// sync, callback infos), 2 Release, which hands it back to the audio system (slot 19) while the
// sound plays on. `play` with no position plays on the local player's own entity, where the
// listener is: heard the same wherever the player goes.
//
// A loop kept at a point (World Architect's placed sounds, start_loop) is such a proxy kept until
// stop_loop, as Audio.loopAt and stopLoop keep one ({ op: 'audioLoopAt', trigger, x, y, z,
// loop }, { op: 'audioLoopStop', loop }: loop the runtime's number for it): slot 8 StopAllTriggers
// (an audio object request 64; slot 7 is StopTrigger(trigger), request 32),
// then Release.
//
// The calls are synchronous requests ({ op: 'syncRequest', id, request: { op: 'audio...' } },
// Client/resources/audio.cjs), answered on the engine thread's next frame with { op:
// 'syncReply', id, value }.
#pragma once

#include <cstdint>
#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::audio {

using Logger = void (*)(const char* format, ...);
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

struct Natives {
  // The entity system's entity of an id, null when there is none.
  void* (*get_entity)(void* entity_system, std::uint32_t id) = nullptr;
  // SAudioCallBackInfos::GetEmptyObject.
  void* empty_callback = nullptr;
  Logger log = nullptr;
};

void install(const Natives& natives);

// A resource runtime message; false when it is not an audio request. Any thread.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Answers the requests queued since the last frame (engine thread, in a world).
void frame(void* entity_system, void* local_entity);

// Sends the answers to the resource runtime (network_mutex held).
void pump(const RuntimeSender& send);

// Engine thread, in a world: one of the game's sound triggers playing at a point until stopped
// (its proxy), null when the trigger is unknown or the audio system is not there.
void* start_loop(const char* trigger, const float position[3]);

// Engine thread: a kept sound moved to a point.
void move_loop(void* loop, const float position[3]);

// Engine thread: a kept sound stopped and its proxy handed back.
void stop_loop(void* loop);

// Engine thread: the resources' kept sounds (Audio.loopAt) stopped, when the world is left.
void stop_resource_loops();

}  // namespace kcds::runtime::audio
