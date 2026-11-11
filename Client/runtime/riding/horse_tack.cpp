#include "horse_tack.hpp"

#include <cstring>

#include "../core/game_layouts.hpp"
#include "MinHook.h"

namespace kcds::runtime::riding::horse_tack {
namespace {

// The inventory screen's own flag of its Horse tab (set by goInHorseTab, cleared by
// goOutHorseTab).
constexpr std::size_t kScreenHorseTab = 2752;
// An item's handle (its WUID), and its class with the class id (a GUID) at + 8.
constexpr std::size_t kItemHandle = kcds::layouts::ItemInstance::kHandle;
constexpr std::size_t kItemClass = kcds::layouts::ItemInstance::kClass;
constexpr std::size_t kClassId = 8;

using Equip = char(__fastcall*)(void* manager, void* item, char flag);
using Unequip = char(__fastcall*)(void* manager, void* item, unsigned count, void* reason,
                                  char flag);
using HorseTabMessage = int(__fastcall*)(void* screen, const char** message);

Natives natives;
Logger log_line = nullptr;
Equip original_equip = nullptr;
Unequip original_unequip = nullptr;
HorseTabMessage original_horse_tab = nullptr;

bool horse_tab_open = false;
// Depth of the outfit sync's own dressing (WhileSyncing).
int syncing = 0;

template <class T>
T read(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

// The server horse a change on this manager is the player's own change of, or 0.
std::uint32_t players_change(void* manager) {
  if (syncing > 0 || !horse_tab_open || !natives.menu_open || !natives.menu_open()) {
    return 0;
  }
  return natives.horse_of_manager ? natives.horse_of_manager(manager) : 0;
}

char __fastcall hooked_equip(void* manager, void* item, char flag) {
  const std::uint32_t horse = item ? players_change(manager) : 0;
  if (!horse) {
    return original_equip(manager, item, flag);
  }
  const auto handle = read<std::uint64_t>(item, kItemHandle);
  std::string row;
  const bool from_bags = natives.bags_row_of && natives.bags_row_of(horse, handle, row);
  const bool known = from_bags || (natives.row_of && natives.row_of(handle, row));
  const bool sent = known && natives.send_equip && natives.send_equip(horse, row, from_bags);
  if (log_line) {
    log_line("HORSE TACK horse=%u equip of %s row %s asked of the server: %s", horse,
             from_bags ? "a saddlebags" : "a player", known ? row.c_str() : "(none)",
             sent ? "sent" : "refused");
  }
  return 0;
}

char __fastcall hooked_unequip(void* manager, void* item, unsigned count, void* reason, char flag) {
  const std::uint32_t horse = item ? players_change(manager) : 0;
  if (!horse) {
    return original_unequip(manager, item, count, reason, flag);
  }
  std::array<std::uint8_t, 16> item_class{};
  const void* type = read<const void*>(item, kItemClass);
  if (type) {
    std::memcpy(item_class.data(), static_cast<const std::uint8_t*>(type) + kClassId,
                item_class.size());
  }
  const bool sent = type && natives.send_remove && natives.send_remove(horse, item_class);
  if (log_line) {
    log_line("HORSE TACK horse=%u removal asked of the server: %s", horse,
             sent ? "sent" : "refused");
  }
  return 1;
}

int __fastcall hooked_horse_tab(void* screen, const char** message) {
  const int result = original_horse_tab(screen, message);
  const bool open = screen && read<std::uint8_t>(screen, kScreenHorseTab) != 0;
  if (open != horse_tab_open) {
    horse_tab_open = open;
    if (log_line) {
      log_line("HORSE TACK the inventory's Horse tab %s", open ? "opened" : "closed");
    }
  }
  return result;
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  const bool equip = hook(natives.equip, reinterpret_cast<void*>(hooked_equip),
                          reinterpret_cast<void**>(&original_equip));
  const bool unequip = hook(natives.unequip, reinterpret_cast<void*>(hooked_unequip),
                            reinterpret_cast<void**>(&original_unequip));
  const bool tab = hook(natives.horse_tab, reinterpret_cast<void*>(hooked_horse_tab),
                        reinterpret_cast<void**>(&original_horse_tab));
  return equip && unequip && tab;
}

WhileSyncing::WhileSyncing() {
  ++syncing;
}

WhileSyncing::~WhileSyncing() {
  --syncing;
}

void tick() {
  if (horse_tab_open && natives.menu_open && !natives.menu_open()) {
    horse_tab_open = false;
    if (log_line) {
      log_line("HORSE TACK the inventory's Horse tab closed with the screen");
    }
  }
}

}  // namespace kcds::runtime::riding::horse_tack
