// The in-game server browser: the main-menu page, the join of the chosen server and
// the watch of the session after it.
#pragma once

#include <atomic>

#include "kcd2s/sdk/value.hpp"

namespace kcds::game_client {

// Winsock and the engine hooks are ready (set by initialize), or initialize gave up.
extern std::atomic<bool> network_ready;
extern std::atomic<bool> adapter_failed;
// Every hook of the client's start is installed and live. On a first start of a new client the
// signatures are scanned, which can outlast a join from the launcher: the server's world waits
// for this.
extern std::atomic<bool> hooks_installed;
// The adapter failed because a game function it needs was not found or could not be hooked: the
// game's build is not one this release supports.
extern std::atomic<bool> game_unsupported;

// Shows the page with the main menu and starts the browser thread.
void start_browser();

// The Leave of World Architect or the Asset/Animation Studio (engine thread): a tool started from
// the main menu leaves its world for the main menu, and the browser thread (ended when the tool
// started) shows the server list again; a game started in the tool closes.
void leave_tool();

// Alt+F4 and the window's close button quit the game.
void quit_on_window_close();

// The ESC menu's "Report a bug" (game thread): the page shows its bug report dialog over the world.
void request_bug_report();

// A client resource's Game.quit() ({ op: 'gameQuit' }): the game closes as the main
// menu's Quit closes it; its departure reaches the server at once. False for another message.
bool handle_game_message(const kcd2s::sdk::Value& message);

}  // namespace kcds::game_client
