// The local player's game input: the gate that keeps it from the game while a UI owns the keyboard,
// the mouse pointer for focused views, the chat's action-map gate and the scripted test input.
#pragma once

namespace kcds::game_client {

// C_PlayerInput::OnAction (hooked: the original).
using PlayerInputAction = void*(__fastcall*)(void*, const char**, unsigned, float);
extern PlayerInputAction original_player_input_action;
void* __fastcall hooked_player_input_action(void* self, const char** action, unsigned mode,
                                            float value);
// The engine's hardware mouse global (gEnv->pHardwareMouse), found at startup.
extern void** hardware_mouse_slot;

// Whether the third-person camera's own mouse listener hears nothing: the UI owns the
// mouse, the pointer is shown, a resource holds the controls or the action rotateyaw.
bool camera_mouse_blocked();
void sync_web_pointer();
void sync_chat_input_gate();
void tick_scripted_input(void* local_actor, double now);
// Every frame (from tick_scripted_input): the game's "sitting_chair" action map off while a
// resource blocks a seat's exits, on again once it does not (input/resource_input.hpp
// sitting_map_held).
void keep_sitting_map();
// A game action pressed on the local player's input, as a key would (the game's own action, by
// name).
void press_player_action(const char* action, unsigned mode, float value);

}  // namespace kcds::game_client
