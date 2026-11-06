// The vendors the server opens (their keepers are server NPC replicas) each frame.
#pragma once

#include <cstdint>

namespace kcds::game_client {

// The engine entity of a server NPC replica by its network id, or null.
void* npc_engine_entity(std::uint32_t network_id);
// The engine entity of a player's replica by their network id, or null.
void* player_engine_entity(std::uint32_t network_id);
// An entity's WUID as the game's GetMyWUID binding gets it.
bool entity_wuid(void* entity, std::uint64_t& wuid);
void tick_vendors(bool in_world);

}  // namespace kcds::game_client
