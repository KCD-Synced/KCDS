// NPCs only this machine shows (the client resources' LocalNpc global;
// a KCDS addition): bodies a resource puts in front of its own player, which the server and the
// other players know nothing of.
//
// A local NPC is a body of the game's own NPC (npc_body.hpp, as a server NPC's body), "NPC_NAI"
// without AI by default or the soul's class with its brain. The resource runtime asks
// (Client/resources/local_npcs.cjs) with localNpcCreate { id, soul, actorClass, name, position,
// rotation }, localNpcPlace { id, position, rotation }, localNpcAppearance { id, appearance },
// localNpcAnimation { id, animation }, localNpcSay { id, speech }, localNpcWalk { id, position,
// speed } (World Architect's NPC preview: the game's own walk, npc_walk.hpp, planned on the
// navmesh) and localNpcRemove { id }; the
// fields are the server NPC's (npc.hpp NpcAppearance, NpcAnimation and NpcSpeech). The requests
// wait for the engine thread; the bodies go when the world unloads or the session ends.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::npcs::local {

using Logger = void (*)(const char* format, ...);

void install(Logger logger);

// A resource runtime message; false when it is not a local NPC's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Engine thread, every frame in the world: makes, moves, dresses, animates and removes the bodies
// as asked, and shows what they say. `view` is the view camera's matrix or null.
void frame(void* actor_system, void* entity_system, const float* view, double now);

// Engine thread, World Architect's local mode (no server NPCs walk there): takes the ended walks
// (npc_walk.hpp take_finished) as the local NPCs' walk states.
void take_walks();

// Network thread: the answers to sync localNpcWalkState { id } ('walking', 'reached', 'no path',
// 'stopped', 'refused' or '' for none).
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

// Every local NPC goes (the world unloads or the session ends). Engine thread.
void clear(void* entity_system);

}  // namespace kcds::runtime::npcs::local
