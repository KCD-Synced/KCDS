// The game's own reaction clips on a replica: the window in which the game may animate a
// replica itself after a replayed reaction, the clips it started then, and the layers given back
// to the owner's clips when the window closes. open_reaction_window is declared in
// core/client_internal.hpp.
#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "../../../Backend/integration/local/animation.hpp"

namespace kcds::game_client {

// A clip the game itself started on a replica while a combat reaction (a block or parry) was
// replayed. The defender's own clip of the same parry arrives a moment later through its owner's
// animations and is then not started again (the attacker saw the defender's parry 150 ms
// after the swords met on the local kit, about 1 s on the internet).
struct ReactionClip {
  int clip = -1;
  ULONGLONG at = 0;
  // The game started it while the replica defended (guard, block or parry) or in a pair: the
  // owner's next clip on the layer may be another variant of the same move. Otherwise only the
  // same clip stands in for the owner's (the game's start of the attacker's swing swallowed
  // its owner's deflection clip 437 ms later, and the parried attacker never recoiled).
  bool variant = false;
};
bool variant_reaction_action(unsigned action);
extern std::mutex reaction_clips_mutex;
extern std::unordered_map<void*, std::array<ReactionClip, 16>> reaction_clips;
extern std::unordered_map<void*, ULONGLONG> reaction_windows;
void return_reaction_layers(void* skeleton, const kcd2s::local::AnimationState& clips,
                            std::uint64_t (&applied)[16]);

}  // namespace kcds::game_client
