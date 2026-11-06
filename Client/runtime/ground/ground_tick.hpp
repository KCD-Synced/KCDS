// The stacks lying in the world that this client sees each frame, and the server's
// rulings on the level's own items. A template over the mirror's entities, so it lives here.
#pragma once

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../hud/item_notice.hpp"
#include "ground_items.hpp"
#include "kcd2s/integration/ground_items.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace kcds::game_client {

// The stacks lying in the world that this client sees (NetworkGroundItem): laid as
// the game's own when they come into view, taken away when they leave it.
template <class Entities>
void tick_ground_items(const Entities& entities) {
  namespace integration = kcd2s::integration;
  std::vector<kcds::runtime::ground::Seen> seen;
  for (const auto& [id, e] : entities) {
    if (e.type != core::type_id("NetworkGroundItem") ||
        e.fields.size() <= integration::GroundDimension)
      continue;
    kcds::runtime::ground::Seen stack;
    stack.id = id;
    const auto position = std::get<core::Vec3>(e.fields[integration::GroundPosition]);
    const auto rotation = std::get<core::Quat>(e.fields[integration::GroundRotation]);
    const auto amount = std::get<std::int64_t>(e.fields[integration::GroundAmount]);
    const auto quality = std::get<std::int64_t>(e.fields[integration::GroundQuality]);
    if (!integration::parse_item_class_script_text(
            std::get<std::string>(e.fields[integration::GroundItemClass]), stack.stack.item) ||
        amount < 1 || amount > integration::kMaxGroundAmount || quality < 0 || quality > 255 ||
        !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
      continue;
    stack.stack.amount = std::uint32_t(amount);
    stack.stack.quality = std::uint8_t(quality);
    stack.stack.health = std::get<float>(e.fields[integration::GroundHealth]);
    stack.stack.position[0] = position.x;
    stack.stack.position[1] = position.y;
    stack.stack.position[2] = position.z;
    stack.stack.rotation[0] = rotation.x;
    stack.stack.rotation[1] = rotation.y;
    stack.stack.rotation[2] = rotation.z;
    stack.stack.rotation[3] = rotation.w;
    stack.resting = std::get<bool>(e.fields[integration::GroundResting]);
    stack.stack.pinned = std::get<bool>(e.fields[integration::GroundPinned]);
    seen.push_back(stack);
  }
  kcds::runtime::ground::sync(seen);
  // Where the stacks that were not resting came to rest in this world.
  const auto rests = kcds::runtime::ground::take_rests();
  // The level's own items: the pick-ups the server ruled on, the items others took up.
  std::vector<std::pair<std::string, bool>> verdicts;
  std::vector<kcd2s::integration::WorldTaken> taken;
  std::optional<bool> items_enabled;
  std::optional<std::vector<kcd2s::integration::RemovedItemArea>> removed_areas;
  std::vector<kcd2s::integration::ItemNotice> notices;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    for (const std::string& key : kcds::runtime::ground::take_world_pickups()) {
      client->peer().send_world_pickup(key);
    }
    namespace sdk = kcd2s::sdk;
    for (const auto& rest : rests) {
      sdk::Value::Map position{{"x", sdk::Value(double(rest.position[0]))},
                               {"y", sdk::Value(double(rest.position[1]))},
                               {"z", sdk::Value(double(rest.position[2]))}};
      sdk::Value::Map rotation{{"x", sdk::Value(double(rest.rotation[0]))},
                               {"y", sdk::Value(double(rest.rotation[1]))},
                               {"z", sdk::Value(double(rest.rotation[2]))},
                               {"w", sdk::Value(double(rest.rotation[3]))}};
      sdk::Value::Map message{{"op", sdk::Value(std::string("groundItemRest"))},
                              {"stack", sdk::Value(std::int64_t(rest.id))},
                              {"position", sdk::Value(std::move(position))},
                              {"rotation", sdk::Value(std::move(rotation))}};
      client->peer().send_script(sdk::Value(std::move(message)));
    }
    verdicts = client->peer().take_world_pickup_verdicts();
    taken = client->peer().take_world_taken();
    items_enabled = client->peer().take_world_items_enabled();
    removed_areas = client->peer().take_world_items_removed();
    notices = client->peer().take_item_notices();
  }
  // What the server put into the player's inventory: the game's own notice.
  for (const auto& notice : notices) {
    const bool shown =
        kcds::runtime::hud::show_item_notice(notice.item, notice.amount, notice.gained);
    log("ITEM notice %s x%u: %s", notice.gained ? "received" : "given", notice.amount,
        shown ? "shown" : "no HUD");
  }
  for (const auto& list : taken) {
    kcds::runtime::ground::world_taken(list.full, list.keys);
  }
  // The server turned the level's items off or on.
  if (items_enabled) {
    kcds::runtime::ground::set_level_items_enabled(*items_enabled);
    log("GROUND level items %s", *items_enabled ? "on" : "off");
  }
  // The places a script took the level's items from (World.removeLevelItems).
  if (removed_areas) {
    kcds::runtime::ground::set_removed_areas(*removed_areas);
    log("GROUND level items removed in %zu places", removed_areas->size());
  }
  kcds::runtime::ground::rule_world_pickups(verdicts);
  kcds::runtime::ground::sweep_world();
}

}  // namespace kcds::game_client
