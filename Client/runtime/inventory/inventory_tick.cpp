#include "inventory_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../ground/ground_items.hpp"
#include "../hud/resource_hud.hpp"
#include "../markers/blips.hpp"
#include "../markers/map_quest_marks.hpp"
#include "../markers/world_markers.hpp"
#include "../quests/journal.hpp"
#include "../quests/quests.hpp"
#include "../riding/horse_saddlebags.hpp"
#include "../riding/horse_tack.hpp"
#include "../stash/stashes.hpp"
#include "../winter/roof_occluders.hpp"
#include "../winter/snow_stage.hpp"
#include "../world/world_clock.hpp"
#include "../world/world_weather.hpp"
#include "inventory_use.hpp"
#include "item_names.hpp"
#include "item_overrides.hpp"
#include "max_column.hpp"
#include "item_types.hpp"
#include "kettles.hpp"
#include "quickslots.hpp"
#include "stock_projection.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace kcds::game_client {

namespace sdk = kcd2s::sdk;

namespace {

// How long a notice in the middle of the screen stays.
constexpr int kCenterNoticeMilliseconds = 3000;

// From hold_local_inventory() until the quickslots redress_local_body() restores are in.
bool holding_reports = false;
bool body_redress_pending = false;
bool body_redressing = false;

}  // namespace

void hold_local_inventory() {
  holding_reports = true;
}

void redress_local_body() {
  body_redress_pending = true;
}

// The inventory the server holds: once the player is in the world the census of what
// their game holds goes to the server, then every stock the server sends is shown and reported.
void tick_inventory(void* local_actor) {
  namespace integration = kcd2s::integration;
  static bool census_sent = false;
  // Item types a resource registered come before the items and names that use them.
  std::vector<integration::ItemTypes> types;
  std::vector<integration::ItemNames> names;
  // The weights, prices and limits the server gives the game's own items, and its
  // notices of a limit the player reached.
  std::optional<integration::ItemOverrides> overrides;
  std::vector<integration::ItemLimitNotice> limit_notices;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    types = client->peer().take_item_types();
    names = client->peer().take_item_names();
    overrides = client->peer().take_item_overrides();
    limit_notices = client->peer().take_item_limit_notices();
  }
  // Shown as the game's own notification or its line in the middle of the screen, as a resource's
  // Hud.showNotification and Hud.showBanner are.
  for (const auto& notice : limit_notices) {
    sdk::Value::Array args{sdk::Value(notice.text)};
    if (notice.center) {
      args.push_back(sdk::Value(std::int64_t(0)));
      args.push_back(sdk::Value(std::int64_t(kCenterNoticeMilliseconds)));
      args.push_back(sdk::Value(false));
    }
    kcds::runtime::hud::handle_runtime_message(sdk::Value(sdk::Value::Map{
        {"op", sdk::Value("hud")},
        {"call", sdk::Value(notice.center ? "showBanner" : "showNotification")},
        {"args", sdk::Value(std::move(args))},
    }));
  }
  for (const auto& update : types) {
    kcds::runtime::inventory::apply_item_types(update);
  }
  kcds::runtime::inventory::apply_item_types({});
  for (const auto& update : names) {
    kcds::runtime::inventory::apply_item_names(update);
  }
  // The local player's soul (the combat actor's at + 1640), whose encumbrance follows the weights.
  void* soul = nullptr;
  if (void* actor = local_combat_actor.load()) {
    std::memcpy(&soul, static_cast<std::uint8_t*>(actor) + 1640, sizeof(soul));
  }
  if (overrides) {
    kcds::runtime::inventory::apply_item_overrides(*overrides, soul, log);
  }
  kcds::runtime::inventory::keep_item_overrides(soul, log);
  kcds::runtime::inventory::tick_max_column(log);
  if (!multiplayer_world.load() || !local_actor) {
    if (census_sent) {
      kcds::runtime::inventory::reset();
      kcds::runtime::inventory::reset_uses();
      kcds::runtime::inventory::reset_item_overrides();
      kcds::runtime::stash::reset();
      kcds::runtime::riding::saddlebags::reset();
    }
    census_sent = false;
    return;
  }
  std::optional<integration::InventoryShow> stock;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    stock = client->peer().take_inventory_show();
  }
  if (!census_sent) {
    integration::InventoryReport census;
    census.kind = integration::InventoryReport::Kind::Census;
    census.rows = kcds::runtime::inventory::census();
    std::lock_guard<std::mutex> lock(network_mutex);
    census_sent = client->peer().send_inventory_report(census);
    if (census_sent) {
      log("INVENTORY census sent: %zu items", census.rows.size());
    }
  }
  static std::uint64_t shown_revision = 0;
  // The quickslots last reported, and last read; none until a stock is shown on this connection.
  static std::optional<integration::InventoryQuickslots> reported_quickslots;
  static std::optional<integration::InventoryQuickslots> seen_quickslots;
  if (!census_sent) {
    reported_quickslots.reset();
    seen_quickslots.reset();
  }
  // Quickslots a resource restored and the frames since their stock was shown: the
  // game's own slots take the stock's rows from the next frame on, once the game put on what the
  // same stock dressed (a belt adds its weapon slots only after it is on, live). A slot the
  // outfit cannot use yet is tried again each frame for at most kQuickslotRestoreFrames; then it is
  // left as the game has it.
  constexpr unsigned kQuickslotRestoreFrames = 60;
  static std::optional<integration::InventoryQuickslots> quickslots_to_restore;
  static unsigned quickslot_restore_frames = 0;
  if (!census_sent) {
    quickslots_to_restore.reset();
    holding_reports = false;
    body_redressing = false;
  }
  if (body_redress_pending) {
    body_redress_pending = false;
    kcds::runtime::inventory::settle_worn(true);
    if (seen_quickslots) {
      quickslots_to_restore = *seen_quickslots;
      quickslot_restore_frames = 0;
      body_redressing = true;
    } else {
      holding_reports = false;
    }
    log("INVENTORY the body is on its new skeleton: what it wore goes back on");
  }
  if (quickslots_to_restore && ++quickslot_restore_frames > 1) {
    const bool last = quickslot_restore_frames >= kQuickslotRestoreFrames;
    unsigned unusable = 0;
    const bool restored = kcds::runtime::inventory::restore_quickslots(
        local_combat_actor.load(), *quickslots_to_restore, last ? log : nullptr, unusable);
    if (!restored || unusable == 0 || last) {
      log("INVENTORY quickslots %s after %u frames", restored ? "restored" : "unavailable",
          quickslot_restore_frames);
      quickslots_to_restore.reset();
      // The next scan reports what the slots hold now.
      reported_quickslots.reset();
      kcds::runtime::inventory::settle_worn();
      if (body_redressing) {
        body_redressing = false;
        holding_reports = false;
      }
    }
  }
  if (stock && kcds::runtime::inventory::show(*stock)) {
    shown_revision = stock->revision;
    if (stock->quickslots) {
      quickslots_to_restore = *stock->quickslots;
      quickslot_restore_frames = 0;
    }
    integration::InventoryReport shown;
    shown.kind = integration::InventoryReport::Kind::Shown;
    shown.revision = stock->revision;
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().send_inventory_report(shown);
  }
  // Stacks lying in the world the player took up: the server decides.
  for (const std::uint32_t stack : kcds::runtime::ground::take_pickups()) {
    integration::InventoryReport pickup;
    pickup.kind = integration::InventoryReport::Kind::Pickup;
    pickup.request = stack;
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().send_inventory_report(pickup);
  }
  for (const auto& dropped : kcds::runtime::ground::take_drops()) {
    integration::InventoryReport drop;
    drop.kind = integration::InventoryReport::Kind::Drop;
    drop.row = dropped.row;
    drop.request = dropped.amount;
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().send_inventory_report(drop);
  }
  // Containers: what the player did, then what the server sent. A lock the
  // player opened goes in its place, before the open that follows it.
  for (const auto& done : kcds::runtime::stash::take_reports()) {
    if (done.unlock) {
      integration::StashUnlock unlock;
      unlock.stash = done.stash;
      unlock.method = done.method;
      std::lock_guard<std::mutex> lock(network_mutex);
      client->peer().send_stash_unlock(unlock);
      continue;
    }
    integration::InventoryReport stash;
    stash.kind = done.kind;
    stash.request = done.stash;
    stash.row = done.row;
    stash.units = done.units;
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().send_inventory_report(stash);
  }
  std::vector<integration::StashShow> containers;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    containers = client->peer().take_stash_shows();
  }
  for (const auto& container : containers) {
    if (container.horse != 0) {
      kcds::runtime::riding::saddlebags::show(container);
    } else {
      kcds::runtime::stash::show(container);
    }
  }
  kcds::runtime::riding::saddlebags::tick();
  kcds::runtime::riding::horse_tack::tick();
  // The world time the server set: kept until the game has its calendar.
  static std::optional<kcd2s::integration::WorldTime> pending_world_time;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (auto sent = client->peer().take_world_time()) {
      pending_world_time = sent;
    }
  }
  if (pending_world_time && kcds::runtime::world::apply(*pending_world_time)) {
    pending_world_time.reset();
  }
  // The weather the server set: held by the weather module until the game has its
  // weather system, and dropped with the world it came from.
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (auto sent = client->peer().take_world_weather()) {
      kcds::runtime::world::hold(*sent);
    }
  }
  kcds::runtime::world::apply_held();
  // The snow stays outside the roof over the camera.
  if (view_matrix_valid) {
    const float camera[3] = {view_matrix[3], view_matrix[7], view_matrix[11]};
    kcds::runtime::winter::snow::shelter(camera);
    kcds::runtime::winter::roof_occluders::update(camera);
  }
  // The server's preset again, before the game's own weather timeline takes over.
  kcds::runtime::world::keep();
  // The markers and blips the server shows or hides, in the order it sent them.
  std::vector<kcd2s::integration::MarkerOrder> marker_orders;
  std::vector<kcd2s::integration::BlipOrder> blip_orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    marker_orders = client->peer().take_marker_orders();
    blip_orders = client->peer().take_blip_orders();
  }
  for (const auto& order : marker_orders) {
    if (order.show) {
      kcds::runtime::markers::show(order.marker);
    } else {
      kcds::runtime::markers::hide(order.marker.marker);
    }
  }
  for (const auto& order : blip_orders) {
    if (order.show) {
      kcds::runtime::markers::show_blip(order.blip);
    } else {
      kcds::runtime::markers::hide_blip(order.blip.blip);
    }
  }
  // The server blips' own images go to the game client through the resource runtime, which has
  // the resources' files.
  for (const auto& [resource, path] : kcds::runtime::markers::take_wanted_icons()) {
    std::lock_guard<std::mutex> lock(network_mutex);
    client->send_to_resource_runtime(sdk::Value(sdk::Value::Map{
        {"op", sdk::Value("mapIconWanted")},
        {"resource", sdk::Value(resource)},
        {"path", sdk::Value(path)},
    }));
  }
  // The client resources' own blips, and whether the compass carries them.
  kcds::runtime::markers::apply_own_blips();
  const auto own_blip_changes = kcds::runtime::markers::take_own_blip_changes();
  if (!own_blip_changes.empty()) {
    std::lock_guard<std::mutex> lock(network_mutex);
    for (const auto& [blip, live] : own_blip_changes) {
      client->send_to_resource_runtime(sdk::Value(sdk::Value::Map{
          {"op", sdk::Value("blipLive")},
          {"blip", sdk::Value(std::int64_t(blip))},
          {"live", sdk::Value(live)},
      }));
    }
  }
  // The quests the server writes into the journal and which of them the player follows (issues),
  // in the order it sent them; what the player then follows goes back to the server
  // and to the resources.
  std::vector<kcd2s::integration::QuestOrder> quest_orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    quest_orders = client->peer().take_quest_orders();
  }
  std::vector<kcds::runtime::quests::TrackingChange> tracking_changes;
  for (const auto& order : quest_orders) {
    kcds::runtime::quests::apply(order, tracking_changes);
  }
  // And what the player changed with the journal's track button.
  for (auto& change : kcds::runtime::quests::take_player_changes()) {
    tracking_changes.push_back(std::move(change));
  }
  if (!tracking_changes.empty()) {
    namespace sdk = kcd2s::sdk;
    std::lock_guard<std::mutex> lock(network_mutex);
    for (const auto& change : tracking_changes) {
      client->send_to_resource_runtime(
          sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("questTrackingChanged"))},
                                     {"key", sdk::Value(change.key)},
                                     {"tracked", sdk::Value(change.tracked)}}));
      client->peer().send_quest_tracking(
          kcd2s::integration::QuestTracking{change.quest, change.tracked});
    }
  }
  // Melee combat a resource ended.
  std::vector<std::uint32_t> cancels;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    cancels = client->peer().take_combat_cancels();
  }
  for (const std::uint32_t opponent : cancels) {
    discard_held_hits("combat cancelled", opponent);
    cancel_combat_with(local_actor, opponent);
  }
  send_player_damage();
  // Uses of usable registered types: the server raises playerCustomItemUse.
  for (const auto& chosen : kcds::runtime::inventory::take_custom_uses()) {
    integration::InventoryReport custom;
    custom.kind = integration::InventoryReport::Kind::CustomUse;
    custom.row = chosen.row;
    custom.revision = chosen.revision;
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().send_inventory_report(custom);
  }
  // Uses the server decides: the questions, then the verdicts and their receipts.
  for (const auto& asked : kcds::runtime::inventory::take_consume_requests()) {
    integration::InventoryReport consume;
    consume.kind = asked.pot ? integration::InventoryReport::Kind::PotConsume
                             : integration::InventoryReport::Kind::Consume;
    consume.request = asked.request;
    consume.row = asked.row;
    consume.revision = asked.revision;
    consume.item = asked.item;
    // The pot the player stands at; its fireplace names it to the server.
    if (asked.pot) {
      const auto pot = kcds::runtime::inventory::pot_at_player();
      if (!pot || !integration::parse_guid(*pot, consume.pot)) {
        consume.pot = {};
      }
      log("INVENTORY pot meal at fireplace %s", pot ? pot->c_str() : "(none in reach)");
    }
    std::lock_guard<std::mutex> lock(network_mutex);
    client->peer().send_inventory_report(consume);
  }
  std::vector<integration::InventoryConsume> verdicts;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    verdicts = client->peer().take_inventory_consumes();
  }
  for (const auto& verdict : verdicts) {
    if (const auto used = kcds::runtime::inventory::apply_consume_verdict(verdict)) {
      integration::InventoryReport receipt;
      receipt.kind = integration::InventoryReport::Kind::Consumed;
      receipt.request = verdict.request;
      receipt.used = *used;
      std::lock_guard<std::mutex> lock(network_mutex);
      client->peer().send_inventory_report(receipt);
    }
  }
  // What the game changed on its own, four times a second.
  if (holding_reports) {
    return;
  }
  constexpr ULONGLONG kScanPeriodMs = 250;
  static ULONGLONG scanned_at = 0;
  const ULONGLONG now = GetTickCount64();
  if (now - scanned_at < kScanPeriodMs) {
    return;
  }
  scanned_at = now;
  // What the current outfit's quickslots hold, when it changed since the last report.
  integration::InventoryReport quickslots;
  quickslots.kind = integration::InventoryReport::Kind::Quickslots;
  if (shown_revision > 0 &&
      kcds::runtime::inventory::read_quickslots(local_combat_actor.load(), quickslots.quickslots)) {
    seen_quickslots = quickslots.quickslots;
    if (!reported_quickslots || *reported_quickslots != quickslots.quickslots) {
      std::lock_guard<std::mutex> lock(network_mutex);
      if (client->peer().send_inventory_report(quickslots)) {
        reported_quickslots = quickslots.quickslots;
        log("INVENTORY quickslots reported: weapon slot %u, item slot %u",
            unsigned(quickslots.quickslots.active_weapon),
            unsigned(quickslots.quickslots.active_item));
      }
    }
  }
  integration::InventoryReport changes;
  changes.kind = integration::InventoryReport::Kind::Changes;
  changes.revision = shown_revision;
  changes.changes = kcds::runtime::inventory::scan();
  if (changes.changes.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  if (client->peer().send_inventory_report(changes)) {
    log("INVENTORY reported %zu changes the game made", changes.changes.size());
  }
}
}  // namespace kcds::game_client
