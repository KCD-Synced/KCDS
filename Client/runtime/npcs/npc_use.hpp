// A press of the use key on a server NPC: the resource host's npcInteract. When the
// local player presses the game's use action, the next frame traces from the view camera along
// where it looks, up to kUseReach metres, bodies included and the player's own skipped
// (physics/line_of_sight.hpp); a hit on the body of a server NPC goes to the server as NPC_USE
// (kcd2s/integration/npc.hpp). The game's own use goes on as it would.
//
// on_action: the input thread's action hook. frame: engine thread.
#pragma once

#include <cstdint>

namespace kcds::runtime::npcs::use {

inline constexpr float kUseReach = 3.5f;

struct Natives {
  // The view camera this frame (3x4 rows: right, forward and up columns 0, 1 and 2, the eye
  // column 3); false outside a world.
  bool (*view_camera)(float matrix[12]) = nullptr;
  // The local player's physical entity, skipped by the trace; null for none.
  void* (*own_physics)() = nullptr;
  // The network id of the server NPC whose body has this engine id here, 0 for none.
  std::uint32_t (*npc_of_engine)(std::uint32_t engine_id) = nullptr;
  // Sends NPC_USE for the NPC; false when it could not go.
  bool (*send)(std::uint32_t npc) = nullptr;
  void (*log)(const char* format, ...) = nullptr;
};

void install(const Natives& natives);

// The local player's game action: a press of "use" is looked at on the next frame.
void on_action(const char* action, unsigned mode);

void frame(bool in_world);

}  // namespace kcds::runtime::npcs::use
