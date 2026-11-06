// Players' bodies carried on the shoulder each frame, and the item carry's put-down
#pragma once

#include <array>
#include <cstdint>

namespace kcds::game_client {

void tick_body_carry(void* entity_system, void* local_actor, void* local_entity);
bool carried_item_position(void* item, float* position);
void report_carry_put_down(const std::array<std::uint8_t, 16>& item_class, const float* position);

}  // namespace kcds::game_client
