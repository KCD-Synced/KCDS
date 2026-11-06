// The server's carry orders to the local player (Player.holdItem and dropHeldItem).
#pragma once

#include <array>
#include <cstdint>

#include "kcd2s/core/math.hpp"

namespace kcds::game_client {

void* lay_carried_item(const std::array<std::uint8_t, 16>& item_class, const float* position);
void tick_carry_orders(void* local_actor, const kcd2s::core::Vec3& position);

}  // namespace kcds::game_client
