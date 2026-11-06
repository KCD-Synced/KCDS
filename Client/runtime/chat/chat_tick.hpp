// The chat each frame: the overlay's input gate, the engine console kept shut while the player
// types, and the messages between the overlay, the server and the resources.
#pragma once

namespace kcds::game_client {

// The console key (~ / `) toggles the console through ShowConsole even while the player types in
// our chat; the console must not open then. Signature: mov rcx,[rdi+230h]; mov esi,r8d; call
// [rax+10h] (the console input check) at the start of CXConsole::ShowConsole.
inline constexpr const char* kShowConsoleSignature =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F9 0F B6 DA 48 8B 89 30 02 "
    "00 00 41 8B F0 48 8B 01 FF 50 10 33 ED 85 C0 8A 87";
using ShowConsoleFn = void(__fastcall*)(void* console, bool show, int scroll);
extern ShowConsoleFn original_show_console;
void __fastcall hooked_show_console(void* console, bool show, int scroll);

void tick_chat();

}  // namespace kcds::game_client
