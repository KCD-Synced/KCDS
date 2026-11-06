// The players' bodies as this game sees them each frame: engine ids and Mannequin tags for the
// resource runtime, and the local player's posture field.
#pragma once

#include <cstdint>

namespace kcds::game_client {

void tick_engine_ids();
// The living server NPCs with a body here (npc_bodies) to the resource runtime as
// { op: 'npcBodies', npcs: [[network id, engine id, x, y, z, name]] }, at most every 250 ms and
// only when the list changed (a body moved over 0.25 m). Engine thread, after tick_native_npcs.
void tick_npc_bodies();
void tick_body_tags(void* local_actor);
std::int64_t tick_posture(void* local_actor);

}  // namespace kcds::game_client
