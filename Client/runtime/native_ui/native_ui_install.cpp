// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "native_ui_install.hpp"

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "native_ui.hpp"

#include "../crafting/recipe_tables.hpp"
#include "../hud/resource_hud.hpp"
#include "../startup_tables/startup_tables.hpp"

namespace kcds::game_client {
namespace {

// A resource's replace/ file the game reads: a perk patch is kept for the next start,
// a recipe patch is loaded now.
bool apply_table_patch(const std::string& path, const std::string& text) {
  if (kcds::runtime::startup_tables::keep(path, text)) {
    return true;
  }
  return kcds::runtime::crafting::recipe_tables::apply(path, text);
}

// A line for the player, as a resource's Hud.showNotification shows it.
void notify_player(const std::string& text) {
  kcd2s::sdk::Value::Array args{kcd2s::sdk::Value(text)};
  kcd2s::sdk::Value::Map message{{"op", kcd2s::sdk::Value(std::string("hud"))},
                                 {"call", kcd2s::sdk::Value(std::string("showNotification"))},
                                 {"args", kcd2s::sdk::Value(std::move(args))}};
  kcds::runtime::hud::handle_runtime_message(kcd2s::sdk::Value(std::move(message)));
}

}  // namespace

void install_native_ui() {
  // NativeUI: the game's own UI elements, as its UIAction script binding drives them.
  kcds::runtime::native_ui::Natives natives;
  natives.string_assign = string_assign;
  natives.string_init =
      resolve_unique("CryStringInitEmpty",
                     "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 C0 0C 48 89 03 48 8B C3 48 "
                     "83 C4 20 5B C3 CC CC 48 89 5C 24 08 48 89 74 24 10 48 89");
  natives.string_free =
      resolve_unique("CryStringFree", "48 83 EC 28 48 8B 09 48 83 C1 F4 E8 ?? ?? ?? ??");
  natives.element = resolve_unique(
      "UIElementInstance",
      "48 89 5C 24 10 48 89 4C 24 08 57 48 83 EC 30 48 8B 0D ?? ?? ?? ?? 41 8B F8 48 85 C9 74 ?? "
      "48 8B 01 4C 8B C2");
  natives.args_init = resolve_unique(
      "UIArgumentsInit",
      "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 E8 ?? ?? ?? ?? 48 83 C0 0C 48 89 "
      "43 08 E8 ?? ?? ?? ?? 48 83 C0 0C 48 8D 4B");
  natives.args_free = resolve_unique(
      "UIArgumentsFree",
      "40 53 48 83 EC 20 48 8B D9 48 83 C1 20 E8 ?? ?? ?? ?? 48 8D 4B 18 E8 ?? ?? ?? ?? 48 8B 4B "
      "10 48 83 E9 0C E8 ?? ?? ?? ?? 48 8D 4B 08 E8 ?? ?? ?? ?? 48 8B CB 48 83 C4 20 5B E9");
  natives.args_add = resolve_unique(
      "UIArgumentsAdd",
      "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 48 8B EC 48 83 EC 40 48 0F BE 42 10");
  natives.data_free =
      resolve_unique("UIDataFree", "48 83 EC 28 48 0F BE 41 10 48 83 C0 01 74 25 48");
  natives.menu_open = resolve_unique("MenuOpen", "48 89 5C 24 20 55 56 57 48 83 EC 20 80 B9 08 28");
  natives.menu_move = resolve_unique(
      "MenuMove", "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 44 88 48 20 57 48 83 EC 20");
  natives.function_free =
      resolve_unique("FunctionFree", "48 89 4C 24 08 48 83 EC 28 48 8B 4C 24 30 E8 ??");
  if (auto open = static_cast<std::uint8_t*>(resolve_unique(
          "GFxFileOpen", "48 89 5C 24 08 57 48 83 EC 30 41 8B D8 48 8D 0D ?? ?? ?? ?? 48 8B FA"))) {
    natives.file_open = open;
    // mov rcx, [rip + allocator] at +0x3B
    if (open[0x3B] == 0x48 && open[0x3C] == 0x8B && open[0x3D] == 0x0D) {
      std::int32_t displacement;
      std::memcpy(&displacement, open + 0x3E, 4);
      natives.file_allocator = reinterpret_cast<void**>(open + 0x42 + displacement);
    }
  }
  if (auto opener = static_cast<std::uint8_t*>(resolve_unique(
          "GFileCryPak",
          "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 C7 41 10 00 00 "
          "00 00 48 89 01 48 8B F1"))) {
    // mov rcx, [rip + ICryPak] at +0x4C
    if (opener[0x4C] == 0x48 && opener[0x4D] == 0x8B && opener[0x4E] == 0x0D) {
      std::int32_t displacement;
      std::memcpy(&displacement, opener + 0x4F, 4);
      natives.pak = reinterpret_cast<void**>(opener + 0x53 + displacement);
    }
    natives.file_create = opener;
  }
  natives.inflate_init = resolve_unique(
      "InflateInit2", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B F2 48 8B D9 4D 85 C0 0F");
  natives.inflate = resolve_unique(
      "Inflate",
      "89 54 24 10 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 48 44 8B C2");
  natives.inflate_end = resolve_unique(
      "InflateEnd", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 85");
  // A resource's rows for the game's recipe tables: the game's load of one table
  // file into a database and the two recipe databases, by the lea of their static
  // constructors.
  kcds::runtime::crafting::recipe_tables::Natives tables;
  tables.load_table_file = resolve_unique(
      "LoadTableFile",
      "48 89 5C 24 08 48 89 6C 24 18 56 57 41 56 48 83 EC 30 41 8A E8 48 8B F2 4C 8B F1 E8 ?? ?? "
      "?? ?? 48 8B 88 70 01 00 00 48 8B 01 FF 90 20 04 00 00");
  tables.databases[0] = resolve_any(
      "BlacksmithRecipeDatabase",
      {{"48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 48 83 EC 38 48 8D 15 72 4A 89 02", 148, 152},
       // The Microsoft Store build.
       {"48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 48 83 EC 38 48 8D 15 ?? ?? ?? 02 48 8D 48 10 "
        "E8 55 16 22",
        148, 152}});
  tables.databases[1] = resolve_any(
      "AlchemyRecipeDatabase",
      {{"48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 48 83 EC 38 48 8D 15 3A 4C 89 02", 136, 140},
       // The Microsoft Store build.
       {"48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 48 83 EC 38 48 8D 15 ?? ?? ?? 02 48 8D 48 10 "
        "E8 1D 18 22",
        136, 140}});
  tables.image = image;
  tables.image_size = image_size;
  tables.string_init = natives.string_init;
  tables.string_free = natives.string_free;
  tables.string_assign = string_assign;
  tables.log = log;
  kcds::runtime::crafting::recipe_tables::install(tables);
  kcds::runtime::startup_tables::install(log, &notify_player);
  natives.table_patch = &apply_table_patch;
  const bool native_ui = kcds::runtime::native_ui::install(natives, log);
  log("NATIVEUI %s", native_ui ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
