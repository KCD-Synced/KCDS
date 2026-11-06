// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "doors_tick.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

#include "doors.hpp"

namespace kcds::game_client {

std::atomic<bool> local_key_bypass{false};

void tick_doors(const Frame& frame) {
  const auto local_entity = frame.local_entity;
  // The level's doors: the server's door states and questions go to the doors
  // module, which reports the doors near the player and the player's own use of them.
  std::vector<kcd2s::integration::DoorState> door_sets;
  std::vector<std::uint32_t> door_questions;
  std::vector<kcd2s::integration::DoorSpawn> door_spawns;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      door_sets = client->peer().take_door_sets();
      door_questions = client->peer().take_door_nearest_requests();
      door_spawns = client->peer().take_door_spawns();
      // player.keyBypass: this game shows every chest unlocked.
      const auto& entities = client->peer().mirror().entities();
      const auto own = entities.find(client->own_avatar());
      local_key_bypass.store(own != entities.end() &&
                             own->second.fields.size() > local::KeyBypass &&
                             std::get<bool>(own->second.fields[local::KeyBypass]));
    }
  }
  kcds::runtime::doors::frame(local_entity, door_sets, door_questions, door_spawns);
}

}  // namespace kcds::game_client
