#include "movement_mode_tick.hpp"

#include "../core/client_internal.hpp"
#include "movement_mode.hpp"
#include "walk_default.hpp"

namespace kcds::game_client {

// Keeps the pace rule the server put on the local player (Player.setPace).
void tick_movement_mode(void* local_actor) {
  kcd2s::integration::MovementMode mode;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    mode = client->peer().movement_mode();
  }
  kcds::runtime::movement::tick(multiplayer_world.load() ? local_actor : nullptr,
                                mode.walk_enforced);
  kcds::runtime::movement::walk_default::tick(multiplayer_world.load() ? local_actor : nullptr,
                                              mode.walk_by_default);
}

}  // namespace kcds::game_client
