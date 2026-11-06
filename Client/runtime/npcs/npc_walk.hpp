// How the simulator's NPC body walks where the server told it (Npc.moveTo):
// the game's own movement task, the one its behaviour trees start, which
// plans the path on the navmesh and walks the body along it with its own animations. Engine
// thread only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::npcs::walk {

// The game's movement natives (WHGame 1.5.5), as game_client.cpp resolves them.
struct Natives {
  // The global whose +120 holds the AI module (its slot 10 is the movement task manager).
  void** ai_module_global = nullptr;
  // The NPC agent of an entity.
  void* agent_of_entity = nullptr;
  // S_MovementTaskRequest's constructor and its cleanup.
  void* request_init = nullptr;
  void* request_cleanup = nullptr;
  // S_PathFindingRequestDefault's constructor.
  void* path_request_init = nullptr;
  // The manager's start of a task for a request and its stop of an agent's.
  void* start_task = nullptr;
  void* stop_tasks = nullptr;
  // The builders of the request's two callbacks (when the task ends, the
  // other), as the game's own walk request calls them.
  void* bind_finished = nullptr;
  void* bind_progress = nullptr;
};

// The game's statuses of a walk that ended (S_MovementTaskResult), as seen live.
inline constexpr std::uint8_t kWalkReached = 0;
// Ended at once: the body is not on a navmesh (a custom map with none, a body up in a tree).
inline constexpr std::uint8_t kWalkOffMesh = 1;
inline constexpr std::uint8_t kWalkNoPath = 3;
inline constexpr std::uint8_t kWalkReplaced = 4;

// A walk that ended: the NPC's network id, the sequence of the order it carried out, and the
// game's status for it.
struct Finished {
  std::uint32_t npc;
  std::uint32_t sequence;
  std::uint8_t status;
};

// The walks that ended since the last call.
std::vector<Finished> take_finished();

// False when one of them is missing; walk_to then does nothing.
bool install(const Natives& natives);

// What the body is told, parsed from the NPC's `intent` field (npc.hpp NpcIntent): a moveTo (its
// destination), a follow (its target), a flee (the point it runs from, in `destination`), a
// lookAt (its target, or with none the point in `destination`), or another order (hold and the
// rest: only its kind).
struct Order {
  std::uint32_t sequence = 0;
  std::string kind;
  float destination[3]{};
  // The followed, looked-at or attacked player's or NPC's network id.
  std::uint32_t target = 0;
  // The game's movement speed: 1 walk, 2 jog, 3 run.
  int speed = 1;
  float radius = 1.5f;
  // An attack with the bow (Npc.attack({ weapon: 'bow' })) rather than in melee.
  bool bow = false;
};
bool parse_order(const std::string& intent, Order& order);

// Starts the body's walk from `from` to `destination` (replacing the walk it had); false when the
// game has no agent for the body or refused the request. Its end is reported for `npc` and the
// order's `sequence` (take_finished).
bool walk_to(void* entity, std::uint32_t npc, std::uint32_t sequence, const float from[3],
             const float destination[3], int speed);

// The game's NPC agent of the body (an IEntity), null when it has none.
void* agent_of(void* entity);

// Stops the body's walk.
void stop(void* entity);

}  // namespace kcds::runtime::npcs::walk
