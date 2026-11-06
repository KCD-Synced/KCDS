// The spoken files the game plays on entities, WHGame 1.5.5: so the line a server NPC says on its
// simulator can be heard on every other client too (npcs/npc_barks.hpp).
//
// The game says a dialogue line or a bark by setting the speaker's audio component's
// switch standalone_file_purpose to the line's purpose (inner, 10 ... 200, how far it
// carries; slot 33 SetSwitchState on aux object 1) and then playing the line's file on the same
// component (slot 28 PlayFile(file, start 0, aux object 1, callback infos)). Every
// entity's audio component is of one class, so both slots are hooked once through any component:
// each file played is kept with its component and the purpose state last set on it.
//
// Test kits (KCDS_TEST_HOOKS) also log every file played, so a line the game says can be named.
// Engine thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::audio::voices {

using Logger = void (*)(const char* format, ...);

// The game's empty audio callback infos (SAudioCallBackInfos::GetEmptyObject), which a
// played file takes.
void set_empty_callback(void* get_empty_object);

// Hooks the component class's PlayFile and SetSwitchState through `component` (an entity's audio
// component), once. `local` is the local player's, named in the test log.
void watch(void* component, void* local, Logger logger);

struct Played {
  void* component = nullptr;
  std::string file;
  // The standalone_file_purpose state set on the component before it (0: none seen).
  std::uint32_t purpose = 0;
  // GetTickCount64 when it played.
  std::uint64_t at = 0;
};

// The files played since the last call.
std::vector<Played> take_played();

// Plays `file` on the component as the game plays a spoken line: the purpose state on the
// standalone_file_purpose switch, then the file, both on aux object 1. False when the component's
// class is not hooked yet, there are no callback infos or the file is empty.
bool play(void* component, const std::string& file, std::uint32_t purpose);

}  // namespace kcds::runtime::audio::voices
