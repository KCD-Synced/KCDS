// The queue of the game's actor LOD budget (wh::entitymodule::C_ActorMonsterLODBudgetting,
// WHGame 1.5.5). The budget keeps two lists of actors waiting for a LOD change, one to raise and
// one to lower, and works through a few of them a frame. A queue beside them names actors that
// must change at once; its update looks each one up in the lowering list, and when it is not
// there takes it out of the raising list without a check. An actor in neither list, or one
// queued twice (its second turn finds it gone), is taken out at the list's end: the move of the
// list's rest runs with a negative length and the game stops (live crash 0xC0000005 in memmove,
// level people's bodies taken while the player skipped time).
//
// Portable: the unit test builds it.
#pragma once

#include <algorithm>
#include <cstddef>

namespace kcds::runtime::replicas {

// Keeps the queued actors the update can take: each once, and only one in one of the two lists.
// The kept ones stay in their order at the start of `queue`; returns how many they are.
inline std::size_t keep_takeable(void** queue, std::size_t count, void* const* raise_begin,
                                 void* const* raise_end, void* const* lower_begin,
                                 void* const* lower_end) {
  std::size_t kept = 0;
  for (std::size_t index = 0; index < count; ++index) {
    void* actor = queue[index];
    const bool listed = std::find(raise_begin, raise_end, actor) != raise_end ||
                        std::find(lower_begin, lower_end, actor) != lower_end;
    const bool taken = std::find(queue, queue + kept, actor) != queue + kept;
    if (listed && !taken) {
      queue[kept] = actor;
      ++kept;
    }
  }
  return kept;
}

}  // namespace kcds::runtime::replicas
