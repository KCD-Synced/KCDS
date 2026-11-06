// The local player's whistle for their horse, heard by the players near them (the server's
// playerWhistle and Player.whistle; WHISTLE in kcd2s/integration/whistle.hpp).
//
// The game's whistle (WHGame 1.5.5): the companion key's "call" action runs the player's horse
// call component (C_Player + 0xD68; the key's own path, which calls the horse); its
// sound and the AI's hearing of it are (component + 32 is the player), hooked here to
// tell the server when the local player whistled. The sound is the voice trigger
// "v_horse_whistle" executed on the player's entity audio proxy (as plays the player's
// feedback sounds): the audio system's trigger id by name (its slot 7), the entity's audio proxy
// (IEntity slot 76, type 3, a shared pointer) and the proxy's ExecuteTrigger (slot 30, with the
// empty callback infos of). A nearby player's whistle plays the same way on their
// replica's entity, so the game's 3D sound fades it with the distance. Engine thread only.
#pragma once

#include <cstdint>
#include <vector>

namespace kcds::runtime::riding::whistle {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* call_horse = nullptr;      // the horse call component's whistle
  void* whistle_sound = nullptr;   // its sound and AI stimulus, hooked
  void* empty_callback = nullptr;  // SAudioCallBackInfos::GetEmptyObject
  void** audio_system = nullptr;   // the global audio system (qword_18492D8B8)
  void* (*local_player)() = nullptr;
  // The replica's entity of a player network id, null when this game has none.
  void* (*replica_entity)(std::uint32_t player) = nullptr;
};

// False when a native is missing or the hook fails.
bool install(const Natives& natives, Logger logger);

// Whether the local player whistled since the last call (to tell the server).
bool take_whistled();

// The server's whistles: 0 the local player whistles (Player.whistle), otherwise that player
// whistled near the local player and their body here plays the whistle.
void apply(const std::vector<std::uint32_t>& whistles);

}  // namespace kcds::runtime::riding::whistle
