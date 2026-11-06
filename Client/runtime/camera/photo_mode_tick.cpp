// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "photo_mode_tick.hpp"

#include <mutex>
#include <optional>

#include "photo_mode.hpp"

namespace kcds::game_client {

void tick_photo_mode_permission() {
  // Whether the server lets the local player open the game's photo mode (Freecam.setEnabled).
  std::optional<bool> freecam;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    freecam = client->peer().take_freecam();
  }
  if (freecam) {
    kcds::runtime::camera::set_photo_mode_allowed(*freecam);
  }
}

}  // namespace kcds::game_client
