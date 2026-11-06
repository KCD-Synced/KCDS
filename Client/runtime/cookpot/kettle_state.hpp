// A pot's fill told to its fireplace's own kettle behaviour, WHGame 1.5.5.
//
// The game decides whether a pot has a meal from its fireplace's kettle state, not from the
// cauldron's look: on E at the pot the fireplace's behaviour (so_fireplace.xml) shows
// "@fireplace_cannotEatFromEmptyKettle" while its persistent state is empty and adds the
// player_eating interrupt to "__player" otherwise, and the pot's trigger offers its meal only
// while that state is neither halfEmpty nor empty (KettleActionTrigger:CanEat). That state is set
// by the message "kettle:signal:onKettleStateChange" with
// "state($enum:kettleState.<full|halfEmpty|empty>)" (the handler in the fireplace's kettleInbox,
// which also sets the cauldron's phase through SetPrefabPhaseById, where the server's look holds).
// So with every server look a pot's fireplace is sent that message the way the script binding
// XGenAIModule.SendMessageToEntity sends one: (unused, the target's WUID,
// type, values) with the game's strings (made from text, freed).
//
// Engine thread only.
#pragma once

#include <cstdint>

namespace kcds::runtime::cookpot::kettle {

using Logger = void (*)(const char* format, ...);
// An entity's WUID as the game's GetMyWUID gets it; false when it has none.
using EntityWuid = bool (*)(void* entity, std::uint64_t& wuid);

struct Natives {
  void* send = nullptr;
  void* make_string = nullptr;
  void* free_string = nullptr;
  EntityWuid wuid_of = nullptr;
  Logger log = nullptr;
};

bool install(const Natives& natives);

// The kettle state of a cauldron's prefab phase: empty for phase 1, full for a pot with meals
// left. The trigger takes halfEmpty for empty (KettleActionTrigger:IsKettleVirtuallyEmpty), so a
// half pot of the server's, which still serves, is told full; its look stays the server's half
// (the kettle's own phase change goes through pot_looks).
const char* state_of_phase(std::uint8_t phase);

// Tells a fireplace (its entity) the fill of its pot's phase, with every look the server sends
// (also after each meal, when the kettle's own count of player meals is reset by it); false when
// the message could not go.
bool tell(void* fireplace, std::uint8_t phase);

// Lights a fireplace's fire (or puts it out) as its own behaviour does when stoked or burnt out:
// the message "kettle:signal:onFireplacePhaseChange" with state($enum:kettleFireState.on|off)
// (so_fireplace.xml), which shows the fire, its steam and smoke. False when the
// message could not go.
bool tell_fire(void* fireplace, bool lit);

// A world left: every fireplace is told again with the next look.
void forget_all();

}  // namespace kcds::runtime::cookpot::kettle
