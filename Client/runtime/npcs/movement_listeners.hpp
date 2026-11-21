// The listeners the navigation module tells when navmesh tiles have streamed in: movement tasks
// (wh::xgenaimodule::navigation::C_MovementTask) waiting for their tiles. A task deleted while it
// waited stayed in the list (its destructor does not take it out), and the next tiles
// streamed called into the freed task: three crashes of one player, after many NPC bodies went.
// Its memory taken by another object by then shows another vtable, which is how such an entry is
// told from a live task.
//
// Portable: the unit test builds it.
#pragma once

#include <cstddef>

namespace kcds::runtime::npcs {

// Keeps, in order, the listeners whose vtable is a movement task's; returns how many there are
// now. vtable_of(listener) is its vtable, or nullptr when its memory cannot be read. A list
// with no movement task at all is not the navigation module's (the game's code that tells it is
// shared with other lists) and is left as it is.
template <class VtableOf>
std::size_t keep_live_tasks(void** listeners, std::size_t count, const void* task_vtable,
                            VtableOf vtable_of) {
  bool any_task = false;
  for (std::size_t index = 0; index < count; ++index) {
    if (listeners[index] && vtable_of(listeners[index]) == task_vtable) {
      any_task = true;
      break;
    }
  }
  if (!any_task) {
    return count;
  }
  std::size_t kept = 0;
  for (std::size_t index = 0; index < count; ++index) {
    void* listener = listeners[index];
    if (listener && vtable_of(listener) == task_vtable) {
      listeners[kept] = listener;
      ++kept;
    }
  }
  return kept;
}

}  // namespace kcds::runtime::npcs
