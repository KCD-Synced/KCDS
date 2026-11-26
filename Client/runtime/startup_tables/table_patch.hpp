// Which of a resource's replace/ files are patches of the game's tables that the game reads only
// when it starts.
#pragma once

#include <string>

namespace kcds::runtime::startup_tables {

// Whether a game path is a patch of one of the game's tables: Libs/Tables/<folder>/<table>__<
// suffix>.xml, at any folder depth (item/item__mymod.xml, Character/CharacterComponent__mymod.xml,
// rpg/perk__myserver.xml). The recipe and workpiece tables are left out: a running game loads
// their patches at once (crafting/recipe_tables).
bool is_table_patch(const std::string& path);

}  // namespace kcds::runtime::startup_tables
