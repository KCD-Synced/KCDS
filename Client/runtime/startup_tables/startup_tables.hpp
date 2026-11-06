// A server's own perks: rows for the game's perk tables that the game reads only when
// it starts.
//
// The game reads each object database once, at its start: its table (Libs/Tables/rpg/perk.xml)
// and then every patch file beside it (perk__<suffix>.xml), whose rows add to or replace the
// table's (C_ObjectTreeDatabase's load, with patch 0, the
// patch list, then with patch 1). The perk database's index of rows is built
// after that, and the game's souls point into its rows, so rows added in a running game would
// leave every learned perk dangling. A server's perks therefore come with the next start:
//
//   - a resource ships the patch files in its replace/ folder (replace/Libs/Tables/rpg/
//     perk__<name>.xml, perk_buff__<name>.xml, ...); when the client mounts them, keep() writes
//     them to %LOCALAPPDATA%\KCDS\startup-tables\<server>\ at their game paths;
//   - at the game's start, right after the engine mounted the mods' packs (before any table is
//     read), mount() packs that folder and mounts it as a mod's pack, so the game's own load
//     finds the patches: the server the launcher starts the game for (KCDS_JOIN_ADDRESS), else
//     the last server joined;
//   - a server whose perk patches this start did not load (another server's start, or new or
//     changed files) has the player told to restart the game once.
//
// mount: engine thread at startup; joining and keep: any thread.
#pragma once

#include <string>

namespace kcds::runtime::startup_tables {

using Logger = void (*)(const char* format, ...);
// Shows the player a line (the HUD's notification).
using Notify = void (*)(const std::string& text);

void install(Logger logger, Notify notify);

// Once a start (the first table load, else after the mods' packs): mounts the kept patches of the
// server this start is for (pak: the engine's CryPak, binding_root: where a mod's packs are bound).
// It runs before install(), so it takes the logger itself.
void mount(void* pak, const char* binding_root, Logger logger);

// Hooks the game's table patch listing, its first call mounting the
// kept patches through the game's CryPak (its global, crypak) before any table's patches are
// listed. As WHGame.dll loads.
void hook_table_loads(void* listing, void* const* crypak, Logger logger);

// The server the player joins ("host:port"): the next start loads its kept patches.
void joining(const std::string& address);

// A replace/ file of a resource the game reads, by its game path: a patch of a perk table is kept
// for the server being joined. True when it is such a patch.
bool keep(const std::string& path, const std::string& text);

// The folder name a server's patches are kept under ("host:port" -> "host_port"), empty for an
// address that makes none.
std::string server_folder(const std::string& address);

// Whether a game path is a patch of a perk table (Libs/Tables/rpg/perk*__*.xml).
bool is_perk_patch(const std::string& path);

}  // namespace kcds::runtime::startup_tables
