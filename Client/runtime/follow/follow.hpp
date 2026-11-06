// The client resources' Follow: the local player follows another
// player or an NPC with the game's own follow action, as the walk-and-talk follow of a
// conversation does (WHGame 1.5.5).
//
// The game's follow (ChatFollowManager, its key chat_focus_follow_init)
// asks the player's action actor (actor + 640) to follow an entity:(action actor,
// &entity id) builds a C_ActorActionFollow (type 30, the entity at + 80) and starts it, unless
// the actor's restriction 26 forbids following. Here the same call starts a script's follow; the
// running follow is the action actor's action of type 30 (its slot 8, as the carry finds its own),
// and a stop finishes that action (C_ActorAction's finish, (action, 2)). The game ends
// it by itself when the target is gone; a movement press ends it here (on_action), since the
// follow steers the player and the game would keep it.
//
// The follow action ends itself on its next update unless the follow manager's check
// accepts the target, and that check only accepts an NPC leading the player. The
// check is hooked to accept the target of the follow this client started. A follow that reached
// its target ends when the target walks off unless finds a conversation partner the
// check accepts; that one is hooked to let this client's follow go on.
//
// The follow's movement (C_ActorActionFollow, its movement provider's request) forces bit
// 26 of the player's movement request, the walking pace: a follower walks behind a leader who
// runs (kit: 9 m behind after a 4 s run, 5 m without the bit). Follow.start's pace
// 'run' clears it for this client's follow, so a led prisoner keeps up with a running guard.
//
// How close the follower keeps (Follow.start distance): the follow's tuning, 64 bytes
// the follow manager hands it for its target (the global follow table + 1000, + 1064 or
// + 1128), holds the distance the follower stops at (+ 16) and the one it sets off again at
// (+ 24); the follow's own check reads them every update. For this client's follow
// with a distance the getter is hooked to hand out a copy that stops at it and sets off 0.5 m past
// it (the game's: stops at 1.5 m, sets off only at 5 m, so a led player trailed 5-8 m behind); the
// game's table is not touched.
//
// The calls are synchronous requests ({ op: 'syncRequest', id, request: { op: 'follow...' } },
// Client/resources/follow.cjs), answered on the engine frame; the follow's start and end go to
// the runtime as { op: 'followEvent', event: 'started' | 'stopped', reason }.
#pragma once

#include <cstdint>
#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::follow {

using Logger = void (*)(const char* format, ...);
using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;

struct Natives {
  void* start = nullptr;               // (action actor, &entity id)
  void* finish = nullptr;              // (action, result)
  void* may_follow = nullptr;          // (follow manager, entity id), hooked
  void* may_keep_following = nullptr;  // (follow manager), hooked
  void* move = nullptr;                // (action, request, input, state), hooked
  void* tuning = nullptr;              // (follow manager, entity id), hooked
  void* (*get_entity)(void* entity_system, std::uint32_t id) = nullptr;
  Logger log = nullptr;
};

void install(const Natives& natives);

// A resource runtime message; false when it is not a follow request. Any thread.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Answers the requests and watches the follow (engine thread, in a world). mounted: the local
// player sits on a horse; a change of it ends the follow ("mountChanged"), and so does a move of
// over 5 m within a frame ("teleported").
void frame(void* entity_system, void* local_actor, void* local_entity, bool mounted);

// The local player's input took an action (mode 1 press): a movement press ends this client's
// follow with the reason "input" (the game's follow steers the player and would keep it). Engine
// thread. True when the action is the follow's to swallow: a follow the player may not end
// (Follow.start playerCanStop false) takes the movement presses and the game's "Stop
// following" key.
bool on_action(const char* action, unsigned mode);

// Sends the answers and the follow's start and end to the runtime (network_mutex held).
void pump(const RuntimeSender& send);

}  // namespace kcds::runtime::follow
