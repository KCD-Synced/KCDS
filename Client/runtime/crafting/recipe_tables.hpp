// A resource's rows for the game's own recipe tables, WHGame 1.5.5.
//
// The game reads each object database once at start: its table (Libs/Tables/<folder>/<Table>.xml)
// and then every patch file next to it (<Table>__<suffix>.xml), whose rows add to or replace the
// table's (C_ObjectTreeDatabase's load). The smithy's recipe list is the database
// wh::playermodule::C_BlacksmithRecipeDatabase (BlacksmithRecipes, minigame), the alchemy table's
// AlchemyRecipe (minigame).
//
// A resource ships a patch file in its replace/ folder (replace/Libs/Tables/minigame/
// BlacksmithRecipes__<name>.xml); once the client mounted it, apply() loads that one file into
// the loaded database as the game loads a patch (database, path, patch) and sorts the
// database again by key (its vtable + 128). Only that database changes: the game's
// own reload of every database (wh_db_ReloadObjectDatabase) breaks a running game. The databases
// are the game's static objects, checked by their classes' vtables (RTTI) before use. The game's
// load waits for work of the engine thread, so apply() runs it on a thread of its own.
//
// Engine thread (apply starts the load on its own thread).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kcds::runtime::crafting::recipe_tables {

using Logger = void (*)(const char* format, ...);
using StringAssign = void*(__fastcall*)(void* string, const char* text);

struct Natives {
  // The loaded WHGame.dll, for the databases' vtables (RTTI).
  const std::uint8_t* image = nullptr;
  std::size_t image_size = 0;
  void* load_table_file = nullptr;  // (database, CryString* path, patch)
  // The game's static databases, BlacksmithRecipes and AlchemyRecipe,
  // as their static constructors name them.
  void* databases[2] = {};
  void* string_init = nullptr;  // CryString's empty constructor
  void* string_free = nullptr;  // CryString's release
  StringAssign string_assign = nullptr;
  Logger log = nullptr;
};

void install(const Natives& natives);

// A file the client mounted for a resource, by its game path, and its text: when it is a patch of
// a recipe table the client knows, its rows are loaded into the game's database. A row's name and
// description may use KCDS's own keys (StringName="kcds_..." with its Text): the client gives the
// game those texts first (hud::set_text), as the game shows a recipe's name by its key. True when
// the load started.
bool apply(const std::string& path, const std::string& text);

}  // namespace kcds::runtime::crafting::recipe_tables
