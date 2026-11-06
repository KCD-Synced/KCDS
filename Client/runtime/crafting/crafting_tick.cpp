#include "crafting_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../native_ui/native_ui.hpp"
#include "../preparing/preparing.hpp"
#include "alchemy.hpp"
#include "permits.hpp"
#include "smithing.hpp"

#include <string>
#include <vector>

namespace kcds::game_client {
namespace {

// The alchemy table the server was last told of, and whether one was.
kcd2s::integration::CraftTable told_table;
bool table_told = false;

}  // namespace

// Opening a crafting station: the local player's asks go to the server and its answers
// open the station or keep it closed. A refusal of request 0, which no ask carries, is the
// server's Crafting.cancel: the station the local player works at closes.
void tick_crafting(void* local_entity) {
  namespace permits = kcds::runtime::crafting::permits;
  const std::uint32_t local_entity_id =
      local_entity
          ? *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(local_entity) + 12)
          : 0;
  std::vector<kcd2s::integration::CraftVerdict> verdicts;
  std::vector<kcd2s::integration::RecipeTable> recipe_tables;
  std::vector<kcd2s::integration::RecipePicture> recipe_pictures;
  bool online = false;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    online = client->peer().world_ready();
    recipe_tables = client->peer().take_recipe_tables();
    recipe_pictures = client->peer().take_recipe_pictures();
    for (const auto& ask : permits::take_asks()) {
      client->peer().send_craft_report(ask);
    }
    // Cooking, smoking and drying the local player's game waits to start.
    for (const auto& ask : kcds::runtime::preparing::take_asks()) {
      client->peer().send_craft_report(ask);
    }
    // The smithing recipe the local player chose goes to the server, which may refuse it.
    float station[3] = {};
    const std::string recipe = kcds::runtime::crafting::smithing::take_chosen_recipe(station);
    if (!recipe.empty() && online) {
      kcd2s::integration::CraftReport report;
      report.kind = kcd2s::integration::CraftReport::Kind::Recipe;
      report.craft = kcd2s::integration::CraftKind::Smithing;
      report.station = {station[0], station[1], station[2]};
      report.reason = recipe;
      client->peer().send_craft_report(report);
    }
    // The game broke the local player's workpiece: the server ends it failed.
    float broken_at[3] = {};
    if (kcds::runtime::crafting::smithing::take_broken_workpiece(broken_at) && online) {
      kcd2s::integration::CraftReport report;
      report.kind = kcd2s::integration::CraftReport::Kind::Failed;
      report.craft = kcd2s::integration::CraftKind::Smithing;
      report.station = {broken_at[0], broken_at[1], broken_at[2]};
      client->peer().send_craft_report(report);
    }
    verdicts = client->peer().take_craft_verdicts();
    // What lies on the alchemy table the local player brews at, when it changed.
    kcd2s::integration::CraftTable table;
    if (kcds::runtime::crafting::alchemy::table_contents(table)) {
      if (online && (!table_told || !(table == told_table)) &&
          client->peer().send_craft_table(table)) {
        told_table = table;
        table_told = true;
      }
    } else {
      table_told = false;
    }
  }
  permits::set_local(local_entity_id, online);
  kcds::runtime::preparing::set_online(online);
  // The recipes resources registered: each a patch of the game's own table.
  for (const auto& table : recipe_tables) {
    // Its own icon first, so the recipe lists find it once the row is in.
    if (!table.icon_name.empty()) {
      kcds::runtime::native_ui::mount_item_icon(table.icon_name, table.icon);
    }
    kcds::runtime::native_ui::mount_table_patch("Libs/Tables/minigame/" + table.file, table.patch);
  }
  // And the alchemy recipes' pictures in the recipe book.
  for (const auto& picture : recipe_pictures) {
    const bool product = picture.which == kcd2s::integration::RecipePictureKind::Product;
    kcds::runtime::native_ui::mount_recipe_picture(product, picture.recipe_id, picture.from,
                                                   picture.dds);
  }
  for (const auto& verdict : verdicts) {
    if (verdict.request == 0 && !verdict.allowed) {
      if (!kcds::runtime::crafting::smithing::close_own()) {
        kcds::runtime::crafting::alchemy::close_own();
      }
      continue;
    }
    if (!kcds::runtime::preparing::apply_verdict(verdict)) {
      permits::apply_verdict(verdict);
    }
  }
}
}  // namespace kcds::game_client
