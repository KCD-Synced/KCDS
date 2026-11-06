#include "placed_objects_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../npcs/native_npcs.hpp"
#include "../props/placed_props.hpp"
#include "../riding/native_horses.hpp"
#include "local_effects.hpp"
#include "placed_effects.hpp"

#include <vector>

namespace kcds::game_client {

// The game entity of a player's, an NPC's or a horse's body by its network id, or null (the
// bursts a server effect plays on a body follow it; a World.addInteraction on a horse
// moves with it).
void* placed_body_entity(std::uint32_t network_id) {
  void* actor = nullptr;
  std::uint32_t engine_id = engine_body_of(network_id, actor);
  if (engine_id == 0) {
    const auto horse = native_horses.find(network_id);
    if (horse != native_horses.end()) {
      engine_id = horse->second.engine_id;
    }
  }
  void* framework = framework_slot ? *framework_slot : nullptr;
  auto actors =
      framework ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304) : nullptr;
  void* entities =
      actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
  return entities && engine_id ? get_entity(entities, engine_id) : nullptr;
}

bool prop_report(const kcd2s::integration::PropReport& report) {
  std::lock_guard<std::mutex> lock(network_mutex);
  return client->peer().send_prop_report(report);
}

// The props and effects the server placed, in the order it sent them, the
// reports of the rigid props this game simulates, and the bursts whose time is over.
void tick_placed_objects() {
  std::vector<kcd2s::integration::PropOrder> props;
  std::vector<kcd2s::integration::VfxOrder> effects;
  std::uint64_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    props = client->peer().take_prop_orders();
    effects = client->peer().take_vfx_orders();
    dropped = client->peer().dropped_placed_orders();
  }
  // Orders lost because too many waited at once: never silent.
  static std::uint64_t dropped_logged = 0;
  if (dropped != dropped_logged) {
    log("PLACED orders dropped: %llu so far (more than %zu waited at once)",
        static_cast<unsigned long long>(dropped), kcd2s::integration::kMaxPlacedOrders);
    dropped_logged = dropped;
  }
  for (const auto& order : props) {
    kcds::runtime::props::placed::apply(order);
  }
  kcds::runtime::props::placed::frame(local::clock_seconds());
  for (const auto& order : effects) {
    kcds::runtime::vfx::placed::apply(order);
  }
  // The effects this game's own scripts play.
  for (const auto& order : kcds::runtime::vfx::local::take_orders()) {
    kcds::runtime::vfx::placed::apply(order);
  }
  kcds::runtime::vfx::placed::tick();
}
}  // namespace kcds::game_client
