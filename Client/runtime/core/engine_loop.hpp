// The client's frame on the engine update thread: the game's update hooked to run frame() after
// it, an engine exception caught and logged with its native stack (the adapter then stops), and a
// NoClip flight ended once after that.
#pragma once

#include <Windows.h>

#include <cstdint>

namespace kcds::game_client {

// The game's update (hooked) and its map callback.
using Update = char(__fastcall*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t);
extern Update original_update;
using MapCallback = std::intptr_t(__fastcall*)(void*);
extern MapCallback map_callback;
char __fastcall hooked_update(void* self, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c);
// Logs an engine exception with its native stack.
int engine_exception(EXCEPTION_POINTERS* details);
LONG CALLBACK observe_engine_exception(EXCEPTION_POINTERS* details);
// The client's frame (game_client.cpp).
void frame();
// Called by initialize_adapter() last of the hooks: the game's update hooked. Returns 0, or 1
// when the hook failed and the adapter must not start (its exit code).
DWORD install_engine_loop_hook();

}  // namespace kcds::game_client
