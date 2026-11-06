// Rob on another player each frame, and where a body stands for it and for the carries.
#pragma once

#include "pickpocket.hpp"

namespace kcds::game_client {

// Where an entity stands and the way it faces, on the ground; false without its matrix.
bool body_of(void* entity, kcds::runtime::pickpocket::Body& body);
void tick_pickpocket(void* entity_system, void* local_actor, void* local_entity);
// The server's pickpocket rules, and the pockets of the players about to be robbed filled into
// their replicas.
void tick_pickpocket_orders(void* entity_system);

}  // namespace kcds::game_client
