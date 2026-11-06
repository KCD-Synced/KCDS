// Blows and shots on server NPCs' bodies, and the blows of NPCs this client simulates on other
// players' replicas.
#pragma once

#include <cstdint>

namespace kcds::game_client {

// Takes the health off a blow on a server NPC's body; true when a replica's blow is dropped.
bool take_npc_hit_health(const std::uint8_t* local, std::uint8_t* hit);

// A server NPC's blow on the local player (a HitInfo: the source's entity id at + 0, the
// target's at + 40, the health it takes at + 48) takes the share of its damage the server set
// (Npc.setCombatSkill({ damage }), npc.hpp NpcDamage); its own simulator's blow and the one
// NPC_BLOW replays alike.
void scale_npc_blow_on_local(const std::uint8_t* local, std::uint8_t* hit);
void relay_npc_blow(const std::uint8_t* hit);

}  // namespace kcds::game_client
