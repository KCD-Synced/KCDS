// World Architect's local mode: the world editor runs only on the author's own
// machine, in the game's developer mode, with no server at all. No server
// and no resource can turn it on.
//
// It is on when the game was started in its developer mode (-devmode on its command line, as the
// development launcher starts it) and the start asked for the editor (KCDS_WORLD_ARCHITECT=1 in its
// environment). Then the client shows no server browser and connects nowhere: its resource runtime
// runs offline (local::Client::set_offline), the client loads the debug world itself on the map
// KCDS_WORLD_ARCHITECT_MAP names (trosecko, kutnohorsko or klaster; trosecko by default) and opens
// the editor when the world is ready. F10 opens it again after Exit.
//
// enabled() any thread; frame() engine thread.
#pragma once

#include <functional>

#include "kcd2s/integration/world_config.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::local_mode {

// Whether this game runs World Architect's local mode: from its start, or since the player chose
// ANIM / ASSETS or WORLD ARCHITECT in the main menu (start_from_menu).
bool enabled();

// The main menu's tool buttons: this game goes into the local mode itself, from its
// main menu, with no new game. The game's developer mode is switched on as -devmode switches it at
// start (CSystem::SetDevMode: the system + 1489 and + 1490; IsDevMode, its slot 127,
// reads + 1490), the tool's variables are set in this process, and the studio's level is handed to
// the game before it loads. Only the player's own click reaches this, never a server. False when
// the game's system is not there yet.
bool start_from_menu(bool studio);

// The world the local mode loads: the debug world without quests on its map.
kcd2s::integration::WorldConfig world();

// { op: 'architectMap', map }: the editor's map switch (trosecko, kutnohorsko or klaster). The
// local world's revision moves on, so the client loads that map's debug world, and the editor
// opens again there. { op: 'architectLeave' }: the editor's Leave (its work saved), served by
// take_leave. True when the message was one of these; any thread.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Whether the editor's screen has drawn since the tool started ({ op: 'editorShown' }, sent by
// both editors once their screen shows): until then KCDS's loading screen covers the world, so
// the player never sees the bare world and HUD first. Any thread.
bool editor_shown();

// Whether to leave now: true once, in the second frame after the editor's Leave came (the props
// it removed first are gone by then); the engine thread, once a frame.
bool take_leave();

// The tool's way out after its Leave: what start_from_menu changed goes back (the developer mode
// flags, the tool's variables, the map switched to), so the client loads no tool world and shows
// its server list again. False, changing nothing, when the game was started in the tool (no
// server list to go back to).
bool stop_from_menu();

// The local world finished loading (the server's world-ready, which the offline peer never
// gets), and whether it has. mark_world_ready on the engine thread; world_ready any thread.
void mark_world_ready();
bool world_ready();

// Each frame of the local mode: in_world once the world is ready (its loading screen may still be
// up: the editor is built under it);
// runtime the resource runtime's generation (a runtime started anew has no editor). Opens the
// editor the first time it is in the world, again in a runtime started anew, and on F10 (the
// game's window in front).
void frame(bool in_world, unsigned runtime,
           const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::local_mode
