// Test hooks for the live kit (KCDS_TEST_HOOKS only): named events the kit sets per process, and
// the engine-thread work they ask for (a probe book, items given or laid, the UI image probe).
// take_test_chat_line, take_probe_lua, signaled and the learn-perk and page-message events are
// declared in core/client_internal.hpp.
#pragma once

#include <Windows.h>

namespace kcds::game_client {

extern HANDLE native_block_test_event;
extern HANDLE book_probe_open_event;
extern HANDLE book_probe_turn_event;
extern HANDLE book_probe_close_event;
extern HANDLE probe_lua_event;
extern HANDLE give_item_event;
extern HANDLE ground_spawn_event;
extern HANDLE chat_line_event;
extern HANDLE body_snapshot_event;
extern HANDLE dice_autoplay_event;

// The UI's picture loader (hooked: the original), logged.
using LoadUiImage = void*(__fastcall*)(void* loader, const char* url);
extern LoadUiImage original_load_ui_image;
void* __fastcall hooked_load_ui_image(void* loader, const char* url);

// Engine thread.
void tick_book_probe();

// Called by initialize_adapter(): the combat test events of this process, the
// scripted input hook, and with KCDS_TEST_HOOKS the test hooks and probes. Returns 0, or 1 when
// a test hook the kit asked for failed and the adapter must not start (its exit code).
DWORD install_test_hooks();
// Called by initialize_adapter() after the engine exception handler: the trample replay event,
// with KCDS_TEST_HOOKS only.
void create_trample_replay_event();

}  // namespace kcds::game_client
