// How the stew pots look, WHGame 1.5.5.
//
// A pot's fill shows as its cauldron's prefab phase. The cauldron is a RuntimePrefab entity its
// fireplace links (proceduralObject['cauldron']); the fireplace's AI behavior (so_fireplace.xml)
// calls the script's Game.SetPrefabPhaseById(cauldron, category, phase) as its kettle state
// changes: category fireplace_cauldron_home or fireplace_cauldron_camp, phase 1 empty and 2 to 7
// goulash, lentil and soup half full and full. That binding (after its Lua argument
// wrapper) checks the entity with the runtime prefab manager (global + 336,
// its set of runtime prefabs at + 8, the find), finds the phase's prefab template in
// the prefab library (() + 224 -> + 296, the lookup taking the category as a
// name built and freeing it) and queues the swap (with the entity
// system's + 8 and { manager, entity id, template }), which only that binding calls; the queue
// swaps at once when it can.
//
// In a server's world the server says each cauldron's phase: it is queued the same way, and the
// game's own phase changes for that cauldron get the server's template instead.
//
// Engine thread only.
#pragma once

#include <cstdint>

namespace kcds::runtime::cookpot {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* binding = nullptr;  // the integer-phase SetPrefabPhaseById
  Logger log = nullptr;
};

bool install(const Natives& natives);

// Shows a cauldron (its entity id) in a phase of the home or camp category; false when the game
// does not have it as a runtime prefab or the phase is not there.
bool show(std::uint32_t entity, bool camp, std::uint8_t phase);

// The server's looks no longer hold (a world left): the game's own phase changes go through.
void release_all();

}  // namespace kcds::runtime::cookpot
