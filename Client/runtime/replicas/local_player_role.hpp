// The local player keeps the game's player role while replicas of other players are made
//
// A player's replica is a C_Player like the local player. When its actor registers with the NPC
// manager (RegisterNPC,in WHGame 1.5.5), an actor of a player class becomes *the*
// player: the manager keeps it at + 96 and its AI object at + 104 (and in the global),
// and its AI object is registered with the player state handler (the handler's player
// at + 584 and its event subscriptions). The handler runs the player's stances: E on a
// seat asked the replica to sit, so the local player got the seated camera but its body stood
// (two clients; alone it sat). When the replica went away, its unregistering
// (which matches manager + 104) would also have taken the role from the local player.
//
// While a replica is being made (the caller's spawning flag), RegisterNPC keeps the manager's
// player, AI object and the global as they were, and the handler registration is skipped. The
// local player's own registration is untouched.
//
// RegisterNPC's player branch also writes the new AI object's id into the global AI
// variable "__player" (with the variable table) and points the AI transform
// tracker (singleton, register) at it. Behaviours take their player from
// "__player": the fire's AddInterrupt asked the NPC manager for the replica's id, so
// the cooking interrupt went to a replica's scheduler, or to nobody once that replica was gone
// After the role is kept, the tracker is moved back to the local player
// (unregister, then register) and "__player" is written again from the manager's
// player, the game's own way.
//
// Every C_Player also has a missile weapon controller (C_MissileWeaponPlayerController at
// player + 3440, its actor at + 24). Its constructor registers the handlers of the
// combat_bow / combat_crossbow / combat_gun actions: one handler per map and action,
// with the controller and its actor's shooting expansion, the last registration replacing the one
// before. Its teardown (from the player's) removes the handlers of those
// actions, whoever registered them. A replica's controller took the local player's bow input:
// aiming ran the replica's controller, whose shooting expansion holds no weapon, and the game
// crashed. The handlers of any controller but the
// local player's (the game's own, GameInterface slot 64) are not registered, and after a
// replica's controller is torn down, the local player's handlers are registered again.
//
// Engine thread.
#pragma once

namespace kcds::runtime::replicas {

using Logger = void (*)(const char* format, ...);

// The natives RegisterNPC's player branch uses for "the player" besides the manager.
struct PlayerIdentityNatives {
  // The global AI variable table (a pointer to it) and the "__player" refresh taking that table.
  void** variables = nullptr;
  void* refresh_player = nullptr;
  // The AI transform tracker: its singleton getter, its register and its unregister.
  void* tracker = nullptr;
  void* tracker_register = nullptr;
  void* tracker_unregister = nullptr;
};

// register_npc: NPCManager::RegisterNPC; register_player_handler: the player state handler's
// registration; identity: the "__player" variable and the transform tracker;
// spawning_replica: true while a replica is being made. False when a native is missing, the
// global cannot be read from RegisterNPC or a hook fails.
bool keep_local_player_role(void* register_npc, void* register_player_handler,
                            const PlayerIdentityNatives& identity, bool (*spawning_replica)(),
                            Logger logger);

// A replica's controller also leaves the bow input alone when its replica's bow is ready (its
// shooting expansion's callback, enables the combat_bow map and adds the controller as
// a listener), and a press of the bow's trigger (controller, shooting expansion) runs
// only for the local player's controller and a weapon that is there (a client crashed on a
// press, reading the expansion's weapon, while another player shot at it).
//
// register_actions: the controller's action handler registration; teardown: its
// teardown; ready: the expansion callback; press: the trigger's press
// local_player: the game's local player, null before it has one. False when a native
// is missing or a hook fails.
bool keep_local_missile_input(void* register_actions, void* teardown, void* ready, void* press,
                              void* (*local_player)());

}  // namespace kcds::runtime::replicas
