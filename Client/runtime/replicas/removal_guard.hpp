// A player's replica removed while it fights, WHGame 1.5.5.
//
// Removing a replica's entity (CEntitySystem::RemoveEntity, slot 19) sends its game object the
// done event, which reaches the C_Player before the CAnimatedCharacter:
// - The C_Player (vtable slot 147,-> ->) tears its combat actor
//   down (C_CombatActor at the actor's + 632). That first calls the actor's "combat
//   actor removed" listeners (actor + 224): there every combat movement animation's spec (a
//   C_CombatActorUpdatedObject registered in; a replica has no network channel, so its
//   spec is a C_CombatAnimActionAIMovement) leaves its combat actor's update list.
//   Then the combat actor's Done (vtable slot 92,->) clears its target, ends
//   its driver's actions and destroys its parts, and at its end takes the objects of types 11 and
//   50 still registered (a movement spec is type 50) off its object list (->).
//   That clears an object's owner (+ 8) but not its update registration (+ 16).
// - The CAnimatedCharacter (slot 1) then releases its action controller,
//   which releases every queued Mannequin action (->).
// The crash's combat movement animation (C_CombatAnimActionMovement, made by a guard movement
// action in and queued) was still queued, never installed, and its spec
// still registered for updates: queued by the teardown itself after the listener call, it lost
// its spec's owner and stayed queued. When the controller released it, the spec's destructor
// unregistered through the cleared owner (locks owner + 0x440): access
// violation on a watcher's game, live. A replica is removed whenever its owner leaves this
// client's area, also in the middle of a fight.
//
// While the adapter removes a replica, the combat module's queue of an animation into that
// replica's action controller is left out, as the game leaves it out itself for a body
// without an action controller: the caller's reference to the animation is released
// and the animation goes with the combat action that made it, while its combat actor still exists
// (a combat action's own object, type 6, is not among the ones Done takes off, so the game ends
// every combat action before that). Every other actor's queue is unchanged.
//
// Engine thread.
#pragma once

namespace kcds::runtime::replicas {

using RemovalLogger = void (*)(const char* format, ...);

// queue_combat_animation: (the combat actor's animation holder, the animation by
// reference, the blend time); module: WHGame.dll's base, for the logged class addresses. False
// when it is missing or the hook failed.
bool install_removal_guard(void* queue_combat_animation, const void* module, RemovalLogger logger);

// From begin_replica_removal until end_replica_removal, the combat animations of `actor` (a
// replica's C_Player, player `player`) are not queued. end_replica_removal returns how many were
// left out.
void begin_replica_removal(void* actor, unsigned player);
unsigned end_replica_removal();

}  // namespace kcds::runtime::replicas
