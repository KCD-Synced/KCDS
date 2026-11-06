#include "siege_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../input/cuffs.hpp"
#include "../knockout/knockout.hpp"
#include "../pillory/pillory.hpp"
#include "siege_engines.hpp"
#include "siege_ladders.hpp"
#include "stone_piles.hpp"

#include <vector>

namespace kcds::game_client {

// The server's siege engines: their spawns, cycle and shots in this game, and the
// local player's asks and the landings this game reports to the server.
// And its siege ladders and stone piles: their spawns, poses and stones, and the local
// player's pushes, raises and throws.
void tick_siege(void* local_actor, void* local_entity) {
  std::vector<kcd2s::integration::SiegeEngineOrder> orders;
  std::vector<kcd2s::integration::SiegeShot> shots;
  std::vector<kcd2s::integration::SiegeLadderOrder> ladder_orders;
  std::vector<kcd2s::integration::StonePileOrder> pile_orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (client) {
      orders = client->peer().take_siege_orders();
      shots = client->peer().take_siege_shots();
      ladder_orders = client->peer().take_siege_ladder_orders();
      pile_orders = client->peer().take_stone_pile_orders();
      for (const auto& use : kcds::runtime::siege::take_uses()) {
        client->peer().send_siege_use(use);
      }
      for (const auto& impact : kcds::runtime::siege::take_impacts()) {
        client->peer().send_siege_impact(impact);
      }
      for (const auto& report : kcds::runtime::siege::ladders::take_reports()) {
        client->peer().send_siege_ladder_report(report);
      }
      for (const auto& report : kcds::runtime::siege::stones::take_reports()) {
        client->peer().send_stone_pile_report(report);
      }
    }
  }
  const double now = local::clock_seconds();
  for (const auto& order : orders) {
    kcds::runtime::siege::apply(order);
  }
  for (const auto& shot : shots) {
    kcds::runtime::siege::fire(shot, now);
  }
  for (const auto& order : ladder_orders) {
    kcds::runtime::siege::ladders::apply(order, now);
  }
  for (const auto& order : pile_orders) {
    kcds::runtime::siege::stones::apply(order);
  }
  const float* local_tm =
      local_entity ? slot<const float*(__fastcall*)(void*)>(local_entity, 32)(local_entity)
                   : nullptr;
  float player[3]{};
  if (local_tm) {
    player[0] = local_tm[3];
    player[1] = local_tm[7];
    player[2] = local_tm[11];
  }
  // No siege use for a player in a menu, in a pillory, in cuffs, in the saddle or knocked out.
  const bool allowed = !ui_owns_input() && !page_over_world.load() && !game_menu_open() &&
                       !kcds::runtime::pillory::in_stocks() &&
                       !kcds::runtime::input::cuffs::cuffed() && !local_rider_seated.load() &&
                       !kcds::runtime::knockout::knocked_out();
  kcds::runtime::siege::frame(now, local_tm ? player : nullptr,
                              view_matrix_valid ? view_matrix.data() : nullptr, allowed);
  // No push or raise from a ladder (IsOnLadder, actor vtable + 1288).
  const bool on_ladder =
      local_actor && slot<bool(__fastcall*)(void*)>(local_actor, kActorIsOnLadder)(local_actor);
  kcds::runtime::siege::ladders::frame(now, local_tm ? player : nullptr, allowed && !on_ladder);
  kcds::runtime::siege::stones::frame(now, local_tm ? player : nullptr,
                                      view_matrix_valid ? view_matrix.data() : nullptr,
                                      allowed && !on_ladder);
}
}  // namespace kcds::game_client
