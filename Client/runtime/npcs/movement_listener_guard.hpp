// The navigation module's tile listeners kept to live movement tasks (movement_listeners.hpp)
// before the game tells them that tiles streamed in: a task deleted while it waited for its tiles
// was called after it was freed, and the game stopped.
//
// Any thread (the game tells the listeners from its navigation jobs).
#pragma once

namespace kcds::runtime::npcs {

using ListenerLogger = void (*)(const char* format, ...);

// notify: the game's function that tells a list of listeners (it reads the list at + 0x158);
// task_vtable: C_MovementTask's vtable. False when either is missing or the hook failed.
bool install_movement_listener_guard(void* notify, const void* task_vtable, ListenerLogger logger);

}  // namespace kcds::runtime::npcs
