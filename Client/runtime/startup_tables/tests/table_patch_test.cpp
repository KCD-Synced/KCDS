#include "../table_patch.hpp"

#include "kcd2s/testkit.hpp"

using kcds::runtime::startup_tables::is_table_patch;

KCD2S_TEST(startup_tables, patches_of_any_table_are_kept) {
  KCD2S_CHECK(is_table_patch("Libs/Tables/rpg/perk__myserver.xml"));
  KCD2S_CHECK(is_table_patch("Libs/Tables/rpg/perk_buff__myserver.xml"));
  KCD2S_CHECK(is_table_patch("Libs/Tables/item/item__mymod.xml"));
  KCD2S_CHECK(is_table_patch("libs\\tables\\character\\CharacterComponent__mymod.xml"));
  KCD2S_CHECK(is_table_patch("Libs/Tables/ai/smartEntity/SmartEntity__mymod.XML"));
}

KCD2S_TEST(startup_tables, whole_tables_and_other_files_are_not_patches) {
  KCD2S_CHECK(!is_table_patch("Libs/Tables/item/item.xml"));
  KCD2S_CHECK(!is_table_patch("Libs/Tables/item__mymod.xml"));
  KCD2S_CHECK(!is_table_patch("Libs/Tables/item/__mymod.xml"));
  KCD2S_CHECK(!is_table_patch("Libs/Tables/item/item__.xml"));
  KCD2S_CHECK(!is_table_patch("Libs/Tables/item/item__mymod.txt"));
  KCD2S_CHECK(!is_table_patch("Objects/item/item__mymod.xml"));
  KCD2S_CHECK(!is_table_patch("Libs/Tables/../Tables/item/item__mymod.xml"));
  KCD2S_CHECK(!is_table_patch("Libs/Tables//item__mymod.xml"));
}

KCD2S_TEST(startup_tables, recipe_tables_load_while_the_game_runs) {
  KCD2S_CHECK(!is_table_patch("Libs/Tables/minigame/AlchemyRecipe__mymod.xml"));
  KCD2S_CHECK(!is_table_patch("Libs/Tables/minigame/BlacksmithRecipes__mymod.xml"));
  KCD2S_CHECK(!is_table_patch("Libs/Tables/minigame/BlacksmithWorkpieces__mymod.xml"));
}
