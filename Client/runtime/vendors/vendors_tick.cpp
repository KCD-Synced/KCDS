#include "vendors_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../npcs/native_npcs.hpp"
#include "../replicas/remote.hpp"
#include "vendor_screen.hpp"

#include <vector>

namespace kcds::game_client {

// The engine entity of a server NPC replica by its network id, or null (a vendor's keeper).
void* npc_engine_entity(std::uint32_t network_id) {
  const auto found = native_npcs.find(core::EntityId(network_id));
  if (found == native_npcs.end() || !framework_slot || !*framework_slot) {
    return nullptr;
  }
  void* actors = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(*framework_slot) + 1304);
  void* entity_system =
      actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
  return entity_system ? get_entity(entity_system, found->second.engine_id) : nullptr;
}
// The engine entity of a player's replica by their network id, or null (a player's trade keeps
// its seller).
void* player_engine_entity(std::uint32_t network_id) {
  const auto found = remotes.find(core::EntityId(network_id));
  if (found == remotes.end() || !framework_slot || !*framework_slot) {
    return nullptr;
  }
  void* actors = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(*framework_slot) + 1304);
  void* entity_system =
      actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
  return entity_system ? get_entity(entity_system, found->second.engine_id) : nullptr;
}
// An entity's WUID as GetMyWUID gets it: its key (vtable slot 2) in the WUID map.
bool entity_wuid(void* entity, std::uint64_t& wuid) {
  void* map = wuid_map_native ? *static_cast<void**>(wuid_map_native) : nullptr;
  if (!entity || !map || !wuid_lookup_native) {
    return false;
  }
  using EntityKey = std::uint64_t(__fastcall*)(void*);
  using WuidLookup = const std::uint64_t*(__fastcall*)(void*, std::uint64_t);
  const std::uint64_t key = slot<EntityKey>(entity, 2)(entity);
  const std::uint64_t* found = reinterpret_cast<WuidLookup>(wuid_lookup_native)(map, key);
  wuid = found ? *found : 0;
  return wuid != 0 && wuid != ~std::uint64_t(0);
}
// The server's vendors (Vendor): their shelves on the game's own shop screen,
// and the baskets the local player confirms there and the screens that go away for the server.
void tick_vendors(bool in_world) {
  std::vector<kcd2s::integration::VendorOrder> orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      orders = client->peer().take_vendor_orders();
    }
  }
  for (const auto& order : orders) {
    kcds::runtime::vendors::apply(order);
  }
  kcds::runtime::vendors::frame(in_world);
  const auto haggles = kcds::runtime::vendors::take_haggles();
  const auto trades = kcds::runtime::vendors::take_trades();
  const auto closes = kcds::runtime::vendors::take_closes();
  if (haggles.empty() && trades.empty() && closes.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  if (!client) {
    return;
  }
  // Haggle offers, then a basket, then the close of its screen (Trade closes the screen).
  for (const auto& haggle : haggles) {
    client->peer().send_vendor_haggle(haggle);
  }
  for (const auto& trade : trades) {
    client->peer().send_vendor_trade(trade);
  }
  for (const auto& closed : closes) {
    client->peer().send_vendor_closed(closed);
  }
}
}  // namespace kcds::game_client
