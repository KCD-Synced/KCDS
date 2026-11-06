// A knocked-out replica's native wake: the native fallen and GetUp observed for its
// owner's wake, the game's ragdoll action kept off a held-down replica, and the wake each frame.
// hooked_push_ragdoll_action is declared in core/client_internal.hpp.
#pragma once

#include "../replicas/remote.hpp"

namespace kcds::game_client {

void observe_replica_wake_restriction(core::EntityId player, void* gate);
void observe_replica_wake_owner(core::EntityId player, Remote& remote, bool down);
bool tick_replica_wake(core::EntityId player, Remote& remote, void* entity);
// Whether this game lays the down replica of player down itself, through its own ragdoll action:
// for its owner's faint, and for a knock-out once the game pushed the action
// (the fall must be as smooth as an NPC's). None of its owner's pose is put on it then.
bool replica_laid_by_game(core::EntityId player);

}  // namespace kcds::game_client
