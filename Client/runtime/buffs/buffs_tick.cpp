#include "buffs_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "custom_buffs.hpp"
#include "player_buffs.hpp"
#include "replica_buffs.hpp"

#include <vector>

namespace kcds::game_client {

// The status effects on the local player's body: the server's orders every frame;
// every 250 ms the body's effects go to the server and to the resource runtime when they changed,
// with the claimed ones the game tried to add.
void tick_buffs(void* local_actor) {
  namespace buffs = kcds::runtime::buffs;
  namespace sdk = kcd2s::sdk;
  constexpr ULONGLONG kBuffsPeriodMs = 250;
  static ULONGLONG read_at = 0;
  static std::vector<kcd2s::integration::BuffEntry> forwarded;
  // The resource runtime they went to: one started anew gets them again.
  static std::uint32_t runtime = 0;
  void* soul = multiplayer_world.load() && local_actor
                   ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 1640)
                   : nullptr;
  std::vector<kcd2s::integration::BuffOrder> orders;
  std::vector<kcd2s::integration::CustomBuff> custom_orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    orders = client->peer().take_buff_orders();
    custom_orders = client->peer().take_custom_buffs();
  }
  buffs::tick(soul);
  buffs::apply(soul, orders);
  // The resources' own buffs (Buffs.register).
  buffs::custom::tick(soul);
  buffs::custom::apply(soul, custom_orders);
  // Their own pictures go to the game client through the resource runtime, which has the
  // resources' files.
  for (const auto& [resource, path] : buffs::custom::take_wanted_images()) {
    std::lock_guard<std::mutex> lock(network_mutex);
    client->send_to_resource_runtime(sdk::Value(sdk::Value::Map{
        {"op", sdk::Value(std::string("buffIconWanted"))},
        {"resource", sdk::Value(resource)},
        {"path", sdk::Value(path)},
    }));
  }
  if (!soul) {
    forwarded.clear();
  }
  const ULONGLONG now = GetTickCount64();
  if (!soul || now - read_at < kBuffsPeriodMs) {
    return;
  }
  read_at = now;
  buffs::custom::keep(soul);
  const auto carried = buffs::read(soul);
  std::vector<kcd2s::integration::BuffEntry> entries;
  for (const auto& buff : carried) {
    entries.push_back(buff.entry);
  }
  const auto blocked = buffs::take_blocked();
  std::lock_guard<std::mutex> lock(network_mutex);
  if (client->peer().send_buffs(entries)) {
    log("BUFFS reported %zu", entries.size());
  }
  for (const auto& buff : blocked) {
    client->peer().send_buff_blocked(buff);
  }
  // The local player's own Player.buffs (only the local player's client has them).
  if (client->resource_runtime_generation() != runtime) {
    runtime = client->resource_runtime_generation();
    forwarded.clear();
  }
  if (entries != forwarded && client->resource_runtime_ready()) {
    sdk::Value::Array list;
    for (const auto& buff : carried) {
      list.push_back(
          sdk::Value(sdk::Value::Array{sdk::Value(kcd2s::integration::guid_text(buff.entry.buff)),
                                       sdk::Value(std::to_string(buff.entry.instance)),
                                       sdk::Value(buff.entry.server), sdk::Value(buff.age)}));
    }
    if (client->send_to_resource_runtime(
            sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("localBuffs"))},
                                       {"buffs", sdk::Value(std::move(list))}}))) {
      forwarded = entries;
    }
  }
}

// The local player's replicaBuffs report: its potions, drinks and injuries, which every other
// client gives its replica, read every 250 ms.
const std::string& tick_replica_buffs(void* local_actor) {
  constexpr ULONGLONG kReadPeriodMs = 250;
  static ULONGLONG read_at = 0;
  static std::string report;
  const ULONGLONG now = GetTickCount64();
  if (!local_actor || now - read_at < kReadPeriodMs) {
    return report;
  }
  read_at = now;
  void* soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 1640);
  const auto buffs = kcds::runtime::buffs::replica::read(soul);
  std::string read_now = kcd2s::integration::replica_buffs_field(buffs);
  if (read_now != report) {
    report = std::move(read_now);
    log("REPLICA BUFFS reported %zu", buffs.size());
  }
  return report;
}
}  // namespace kcds::game_client
