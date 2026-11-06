// The AI LOD manager and player replicas, WHGame 1.5.5.
//
// The game's AI LOD manager (wh::xgenaimodule::C_LODManager, the singleton) keeps an
// entry per NPC actor; when an actor's update flags change (for example when the game
// picks a body up onto someone's shoulder) it looks the actor's entry up (through the
// thunk) and uses it without a check. A player's replica (the C_Player
// class) has no AI and no entry, so a carry of a knocked-out replica read through the null entry
// (live crash 0xC0000005). The update is hooked so an actor without an entry is left
// out of it; every actor with one goes on as before.
//
// Engine thread.
#pragma once

namespace kcds::runtime::replicas {

using LodLogger = void (*)(const char* format, ...);

// lod_update: (manager, the entity key, flag). False when it is missing or the hook
// failed.
bool install_lod_guard(void* lod_update, LodLogger logger);

}  // namespace kcds::runtime::replicas
