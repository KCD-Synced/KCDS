#include "kcd2s/integration/crafting_table.hpp"

#include "kcd2s/testkit.hpp"
using namespace kcd2s::integration;

// CRAFT_TABLE.

KCD2S_TEST(crafting_table, round_trip) {
  CraftTable table;
  CraftTableItem water;
  water.position = 6;
  water.base = true;
  CraftTableItem herb;
  herb.position = 2;
  for (std::size_t i = 0; i < herb.item.size(); ++i) {
    herb.item[i] = std::uint8_t(i * 7);
  }
  herb.milled = true;
  herb.distilled = true;
  table.items = {water, herb};
  CraftTable decoded;
  KCD2S_CHECK(decode_craft_table(encode_craft_table(table), decoded) && decoded == table);
  CraftTable empty;
  KCD2S_CHECK(decode_craft_table(encode_craft_table(empty), decoded) && decoded.items.empty());
}

KCD2S_TEST(crafting_table, refuses_what_it_cannot_carry) {
  CraftTable table;
  CraftTableItem item;
  item.position = kCraftTablePositions;
  table.items = {item};
  KCD2S_CHECK(encode_craft_table(table).empty());
  table.items.assign(kMaxCraftTableItems + 1, CraftTableItem{});
  KCD2S_CHECK(encode_craft_table(table).empty());
  table.items = {CraftTableItem{}};
  auto bytes = encode_craft_table(table);
  CraftTable decoded;
  auto flags = bytes;
  flags.back() = 8;
  KCD2S_CHECK(!decode_craft_table(flags, decoded));
  auto longer = bytes;
  longer.push_back(0);
  KCD2S_CHECK(!decode_craft_table(longer, decoded));
  KCD2S_CHECK(!decode_craft_table({}, decoded));
}
