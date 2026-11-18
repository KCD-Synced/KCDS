// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "aim_scripts.hpp"

namespace kcds::game_client {

void tell_scripts_aiming(bool aiming) {
  namespace sdk = kcd2s::sdk;
  static bool told = false;
  static bool told_aiming = false;
  static std::uint32_t runtime = 0;
  const bool now = multiplayer_world.load() && aiming;
  std::lock_guard<std::mutex> lock(network_mutex);
  if (client->resource_runtime_generation() != runtime) {
    runtime = client->resource_runtime_generation();
    told = false;
  }
  if ((told && told_aiming == now) || !client->resource_runtime_ready()) {
    return;
  }
  if (client->send_to_resource_runtime(sdk::Value(sdk::Value::Map{
          {"op", sdk::Value(std::string("localAim"))}, {"aiming", sdk::Value(now)}}))) {
    told = true;
    told_aiming = now;
  }
}

}  // namespace kcds::game_client
