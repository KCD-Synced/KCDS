// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "gathering_tick.hpp"

#include <vector>

#include "herbs.hpp"

namespace kcds::game_client {

// The local player's picks go to the server; its answers and the world's picks and regrowth come
// to the game's plants.
void tick_gathering() {
  namespace herbs = kcds::runtime::gathering;
  std::vector<kcd2s::integration::GatherVerdict> verdicts;
  std::vector<kcd2s::integration::GatherPlants> plants;
  bool online = false;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    online = client->peer().world_ready();
    for (const auto& report : herbs::take_reports()) {
      client->peer().send_gather_report(report);
    }
    verdicts = client->peer().take_gather_verdicts();
    plants = client->peer().take_gather_plants();
  }
  herbs::set_online(online);
  for (const auto& verdict : verdicts) {
    herbs::apply_verdict(verdict);
  }
  for (const auto& batch : plants) {
    herbs::apply_plants(batch);
  }
}

}  // namespace kcds::game_client
