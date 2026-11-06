// Two players on one ladder (two may share a long ladder, but their
// bodies must not overlap).
//
// The game keeps two NPCs off one ladder itself (asks its ladder manager whether the top
// or the bottom is taken), but a replica is not a native ladder user, so nothing kept the local
// player out of a remote player's body on a ladder. The owner reports that it is on a ladder (the
// posture bit kPostureOnLadder, from its game's IsOnLadder, actor vtable + 1288), with its
// replicated position. In every other game:
//
// - The local player's grab of a ladder is refused while a remote player on it is within a body
//   (2 m along the ladder) of where the grab starts, its foot or its top; the player waits there.
//   The grab has two ways in: GrabOnLadder (self, whose + 8 slot 2 is the actor;
//   the ladder's entity id), which starts the player's ladder action and names the ladder, so the
//   remote player is the one within 1 m of that ladder's axis (its entity's up axis from its foot);
//   and the ladder helper's own (the helper, whose + 56 is the actor), which NPCs go
//   through, where it is one within 1.6 m across of the local player.
// - The game's own check whether the local player may use a ladder (CanUseLadder:
//   the actor, the ladder's entity, its height, which end, a flag; it asks the ladder manager
//   whether the end is taken, the way NPCs keep apart) says no for the same reason, so the climb
//   hint is not offered while the grab would be refused.
// - On a ladder, the local player's climb towards a remote player within a body is held back: a
//   press of that way (moveforward up, moveback down) does not go to the game, and a climb under
//   way is let go of through the player's input as a key release would be.
//
// Engine thread.
#ifndef KCDS_RUNTIME_MOVEMENT_SHARED_LADDERS_HPP
#define KCDS_RUNTIME_MOVEMENT_SHARED_LADDERS_HPP

#include <array>
#include <cstdint>
#include <vector>

namespace kcds::runtime::movement::ladders {

using Logger = void (*)(const char* format, ...);

// An entity's world matrix (3 x 4, rows), false when there is no such entity.
using EntityMatrix = bool (*)(std::uint32_t entity, float* matrix);

// The player input's OnAction: (the player input, the action id, mode, value).
using Deliver = void*(__fastcall*)(void* input, const char** action, unsigned mode, float value);

// helper_grab:, grab_by_id:, can_use:. False when one
// could not be hooked.
bool install(void* helper_grab, void* grab_by_id, void* can_use, EntityMatrix entity_matrix,
             Logger logger);

// The local player's input takes an action (mode 1 press, 2 release): true when it must not go to
// the game (a climb into a remote player).
bool refuse_action(void* input, const char** action, unsigned mode);

// Every frame: the local player's actor, where it stands and whether it is on a ladder, and where
// each remote player who is on a ladder is (its network id and position).
struct Climber {
  std::uint32_t player = 0;
  std::array<float, 3> position{};
};
void tick(void* local_actor, const std::array<float, 3>& position, bool on_ladder,
          std::vector<Climber> climbers, Deliver deliver);

}  // namespace kcds::runtime::movement::ladders

#endif  // KCDS_RUNTIME_MOVEMENT_SHARED_LADDERS_HPP
