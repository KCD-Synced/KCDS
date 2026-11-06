// No game mods while playing KCDS: a player's own game mods (a cheat pak in the
// game's Mods folder, a Steam Workshop mod) stay off, whatever is installed, without changing any
// file of the game, so the same game runs its mods again without KCDS. KCDS's own assets are
// mounted by the client itself (branding/runtime_branding.hpp), not as a game mod.
//
// WHGame 1.5.5: the game's mod manager (wh::C_ModManager, CrySystem/Mods/ModManager.cpp) finds
// its mods in as the game starts: the platform's mods (Steam Workshop), then every
// folder of Mods/ ("[Mod] Loading mods in alphabetical order") into its list of mods at
// + 24 / + 32 (112 bytes each), then the enabled ones into the list at + 48 / + 56 (pointers into
// the first): the ones mod_order.txt names or, without it, all of them. Only enabled
// mods get their paks opened and their localization patches loaded. The hook lets the game find
// its mods (so its own report stays right) and empties the enabled list after it.
//
// Installed as soon as WHGame.dll is mapped (session/early_hooks.cpp), before the game finds its
// mods.
#pragma once

namespace kcds::runtime::game_mods {

using Logger = void (*)(const char* format, ...);
// Returns the unique match of a byte signature in WHGame.dll, or null.
using Resolver = void* (*)(const char* name, const char* pattern);

// Hooks the mod manager's discovery; false when its signature does not match (the game's mods
// then load as without KCDS, which is logged).
bool install(Resolver resolve, Logger log);

}  // namespace kcds::runtime::game_mods
