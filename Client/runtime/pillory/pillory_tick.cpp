// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "pillory_tick.hpp"

#include <mutex>
#include <vector>

#include "pillory.hpp"

namespace kcds::game_client {

void tick_pillory(const Frame& frame) {
  const auto local_actor = frame.local_actor;
  const auto local_entity = frame.local_entity;
  // The server's Player.putInPillory and releaseFromPillory for the local player,
  // done through the game's unstance "Pillory" (pillory/pillory.hpp); a lock pick that opened
  // the pillory's lock, and a release the client did, go back to the server.
  std::vector<kcd2s::integration::PilloryOrder> orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      orders = client->peer().take_pillory_orders();
    }
  }
  std::vector<kcd2s::integration::PilloryReport> reports;
  kcds::runtime::pillory::frame(local_actor, local_entity, orders, reports);
  for (const auto& report : reports) {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      client->peer().send_pillory_report(report);
    }
  }
}

}  // namespace kcds::game_client
