#include "row_names.hpp"

#include <cstdint>
#include <cstring>
#include <string>

#include "MinHook.h"
#include "item_names.hpp"
#include "stock_projection.hpp"

namespace kcds::runtime::inventory {
namespace {

// The item's handle; the entry's and the panel's name and the panel's description strings.
constexpr std::size_t kItemHandle = 0x30;
constexpr std::size_t kEntryName = 16;
constexpr std::size_t kPanelDescription = 40;

using MakeEntry = void*(__fastcall*)(void* entry, void* item, void* pricer);
using StringAssign = void*(__fastcall*)(void* string, const char* text);

Logger log_line = nullptr;
StringAssign assign = nullptr;
std::string (*horse_piece_key)(void* item) = nullptr;
MakeEntry original_list_entry = nullptr;
MakeEntry original_info_panel = nullptr;

// The server's row an item shows; empty for an item no row keeps.
std::string row_shown_by(void* item) {
  std::string row;
  if (!item) {
    return row;
  }
  std::uint64_t handle = 0;
  std::memcpy(&handle, static_cast<std::uint8_t*>(item) + kItemHandle, sizeof(handle));
  std::uint64_t revision = 0;
  if (!row_of(handle, row, revision)) {
    row.clear();
  }
  return row;
}

// Puts the text the server gave a row's own key into a string of the entry. The entry's strings
// are shown as they are, not looked up as keys (a key showed as written).
void use_key(void* made, std::size_t offset, const std::string& key) {
  std::string text;
  if (item_name_of(key, text)) {
    assign(static_cast<std::uint8_t*>(made) + offset, text.c_str());
  }
}

// The suffix of the keys an item's own name and description are under: "r_<row>" for a row of
// the local player's, "h_<horse>_<class>" for a piece a server horse wears; empty for neither.
std::string keys_of(void* item) {
  const std::string row = row_shown_by(item);
  if (!row.empty()) {
    return "r_" + row;
  }
  if (item && horse_piece_key) {
    const std::string piece = horse_piece_key(item);
    if (!piece.empty()) {
      return "h_" + piece;
    }
  }
  return {};
}

void* __fastcall hooked_list_entry(void* entry, void* item, void* pricer) {
  void* made = original_list_entry(entry, item, pricer);
  const std::string keys = keys_of(item);
  if (made && !keys.empty()) {
    use_key(made, kEntryName, "kcds_nm_" + keys);
  }
  return made;
}

void* __fastcall hooked_info_panel(void* panel, void* item, void* pricer) {
  void* made = original_info_panel(panel, item, pricer);
  const std::string keys = keys_of(item);
  if (made && !keys.empty()) {
    use_key(made, kEntryName, "kcds_nm_" + keys);
    use_key(made, kPanelDescription, "kcds_in_" + keys);
  }
  return made;
}

bool hook(void* target, void* detour, MakeEntry* original) {
  return target && MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install_row_names(const RowNameNatives& natives, Logger logger) {
  log_line = logger;
  assign = reinterpret_cast<StringAssign>(natives.string_assign);
  horse_piece_key = natives.horse_piece_key;
  if (!assign) {
    return false;
  }
  const bool entries =
      hook(natives.list_entry, reinterpret_cast<void*>(&hooked_list_entry), &original_list_entry);
  const bool panels =
      hook(natives.info_panel, reinterpret_cast<void*>(&hooked_info_panel), &original_info_panel);
  if (log_line) {
    log_line("INVENTORY row names: list entries %s, item panel %s",
             entries ? "hooked" : "not hooked", panels ? "hooked" : "not hooked");
  }
  return entries && panels;
}

}  // namespace kcds::runtime::inventory
