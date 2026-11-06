// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "graves_tick.hpp"

#include <mutex>
#include <vector>

#include "graves.hpp"

namespace kcds::game_client {

void tick_graves(const Frame& frame) {
  // The level's graves: the server's states go to the graves module, which reports
  // the level's graves and the local player's digging.
  std::vector<kcd2s::integration::Grave> sets;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      sets = client->peer().take_grave_sets();
    }
  }
  const auto out = kcds::runtime::graves::frame(frame.now, sets);
  if (out.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  if (client) {
    for (const auto& grave : out) {
      client->peer().send_grave(grave);
    }
  }
}

}  // namespace kcds::game_client
