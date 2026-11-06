// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "weapon_hits_tick.hpp"

#include <mutex>
#include <utility>
#include <vector>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../ground/ground_items.hpp"
#include "../physics/local_world.hpp"
#include "../physics/world_raycast.hpp"
#include "../weapons/drawn_weapons.hpp"
#include "weapon_hits.hpp"

namespace kcds::game_client {

namespace {

// The hits the runtime has not had yet; a burst keeps the oldest kMostWaiting.
constexpr std::size_t kMostWaiting = 64;
std::mutex waiting_mutex;
std::vector<kcd2s::sdk::Value> waiting;

}  // namespace

void install_weapon_hits() {
  kcds::runtime::weapon_hits::Natives natives;
  natives.contact_check = resolve_unique(
      "WeaponSolverContactCheck",
      "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 83 EC 40 4C 8B 01 48 8B D9 0F 29 70 E8 "
      "0F 29 78 D8 0F 28 F9 49 8B 00");
  natives.component = resolve_unique("EntityComponent", "4C 8B 81 A8 00 00 00 45 33 D2 49 8B C8");
  const bool installed = kcds::runtime::weapon_hits::install(natives, log);
  log("WEAPON HITS %s", installed ? "installed" : "unavailable");
}

void tick_weapon_hits(const Frame& frame) {
  void* item =
      frame.local_actor ? kcds::runtime::weapons::item_in_hand(frame.local_actor, false) : nullptr;
  const std::uint32_t id = item ? kcds::runtime::ground::entity_of(item) : 0;
  void* weapon = id && frame.entity_system ? get_entity(frame.entity_system, id) : nullptr;
  const auto contacts =
      kcds::runtime::weapon_hits::tick(frame.local_actor, frame.local_entity, weapon, frame.now);
  if (contacts.empty()) {
    return;
  }
  std::vector<kcd2s::integration::WorldRayHit> hits;
  {
    std::lock_guard<std::mutex> lock(waiting_mutex);
    for (const auto& contact : contacts) {
      auto hit = kcds::runtime::physics::server_hit(contact.hit, contact.distance);
      // A body is never a weapon hit: the trace does not meet living bodies.
      hit.entity = 0;
      hits.push_back(std::move(hit));
      if (waiting.size() < kMostWaiting) {
        waiting.push_back(kcds::runtime::physics::script_hit(contact.hit, contact.distance));
      }
    }
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  if (!client) {
    return;
  }
  for (const auto& hit : hits) {
    client->peer().send_weapon_hit(hit);
  }
}

void pump_weapon_hits(const std::function<bool(const kcd2s::sdk::Value&)>& send) {
  std::vector<kcd2s::sdk::Value> taken;
  {
    std::lock_guard<std::mutex> lock(waiting_mutex);
    taken.swap(waiting);
  }
  for (auto& hit : taken) {
    send(kcd2s::sdk::Value(kcd2s::sdk::Value::Map{
        {"op", kcd2s::sdk::Value(std::string("weaponHit"))}, {"hit", std::move(hit)}}));
  }
}

}  // namespace kcds::game_client
