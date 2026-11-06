#include "horse_saddlebags.hpp"

#include <cstring>
#include <unordered_map>
#include <vector>

#include "../inventory/stock_projection.hpp"
#include "horse_tack.hpp"
#include "MinHook.h"

namespace kcds::runtime::riding::saddlebags {
namespace {

namespace integration = kcd2s::integration;

// An item: its handle, its amount, and the inventory it is in.
constexpr std::size_t kItemHandle = 0x30;
constexpr std::size_t kItemAmount = 0x50;
constexpr std::size_t kItemInventory = 144;

using MoveItem = void*(__fastcall*)(void* target, void* item, unsigned units, char flag);

struct Bags {
  std::uint32_t stash = 0;
  std::vector<integration::InventoryRowState> rows;
  // The body and inventory the rows were last put into, and the rows by the items' handles.
  void* entity = nullptr;
  void* inventory = nullptr;
  std::unordered_map<std::uint64_t, std::string> shown;
  bool changed = true;
  // No longer the local player's (a closed show): let go once the inventory is emptied.
  bool closed = false;
};

Natives natives;
Logger log_line = nullptr;
MoveItem original_move = nullptr;
// Horse network id -> its saddlebags.
std::unordered_map<std::uint32_t, Bags> bags;

template <class Value>
Value at(const void* object, std::size_t offset) {
  Value value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

bool ask(bool take, std::uint32_t stash, const std::string& row, std::uint32_t units) {
  return natives.send && natives.send(take, stash, row, units);
}

// A move between the local player and a horse whose saddlebags the server holds is asked of the
// server; every other goes on.
void* __fastcall hooked_move(void* target, void* item, unsigned units, char flag) {
  void* source = item ? at<void*>(item, kItemInventory) : nullptr;
  void* player = source && target ? inventory::local_player_inventory() : nullptr;
  if (!player || (source != player && target != player)) {
    return original_move(target, item, units, flag);
  }
  for (const auto& [horse, held] : bags) {
    if (!held.inventory || (target != held.inventory && source != held.inventory)) {
      continue;
    }
    const auto handle = at<std::uint64_t>(item, kItemHandle);
    const std::uint32_t moved = units != 0 ? units : at<std::uint32_t>(item, kItemAmount);
    std::string row;
    bool ask_put = false;
    if (target == held.inventory) {
      std::uint64_t revision = 0;
      if (!inventory::row_of(handle, row, revision)) {
        if (log_line) {
          log_line("SADDLEBAGS horse %u: an item no server row keeps stays with the player", horse);
        }
        return item;
      }
      ask_put = true;
    } else {
      const auto found = held.shown.find(handle);
      if (found == held.shown.end()) {
        return item;
      }
      row = found->second;
    }
    const bool sent = ask(!ask_put, held.stash, row, moved);
    if (log_line) {
      log_line("SADDLEBAGS horse %u: %s %u of %s %s", horse, ask_put ? "put" : "take", moved,
               row.c_str(), sent ? "asked" : "not sent");
    }
    return item;
  }
  return original_move(target, item, units, flag);
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.move_item || !natives.horse_entity) {
    return false;
  }
  return MH_CreateHook(natives.move_item, reinterpret_cast<void*>(hooked_move),
                       reinterpret_cast<void**>(&original_move)) == MH_OK &&
         MH_EnableHook(natives.move_item) == MH_OK;
}

void show(const integration::StashShow& shown) {
  if (shown.horse == 0) {
    return;
  }
  Bags& held = bags[shown.horse];
  held.stash = shown.stash;
  held.rows = shown.closed ? std::vector<integration::InventoryRowState>{} : shown.rows;
  held.changed = true;
  held.closed = shown.closed;
  if (log_line) {
    log_line("SADDLEBAGS horse %u: %zu rows%s", shown.horse, held.rows.size(),
             shown.closed ? " (no longer this player's)" : "");
  }
}

void tick() {
  if (!natives.horse_entity) {
    return;
  }
  for (auto& [horse, held] : bags) {
    void* entity = natives.horse_entity(horse);
    if (!entity) {
      held.entity = nullptr;
      held.inventory = nullptr;
      continue;
    }
    if (entity == held.entity && !held.changed) {
      continue;
    }
    void* inventory = inventory::inventory_of_entity(entity);
    if (!inventory) {
      continue;
    }
    held.entity = entity;
    held.inventory = inventory;
    void* wearer = natives.horse_actor ? natives.horse_actor(horse) : nullptr;
    // Not the player's own tack change, whatever the game takes off meanwhile.
    horse_tack::WhileSyncing filling;
    held.shown = inventory::fill_container(inventory, held.rows, wearer);
    held.changed = false;
  }
  // A closed show's horse is let go once its inventory was emptied.
  for (auto it = bags.begin(); it != bags.end();) {
    if (it->second.closed && !it->second.changed) {
      it = bags.erase(it);
    } else {
      ++it;
    }
  }
}

void reset() {
  bags.clear();
}

bool row_of(std::uint32_t horse, std::uint64_t handle, std::string& row) {
  const auto found = bags.find(horse);
  if (found == bags.end()) {
    return false;
  }
  const auto shown = found->second.shown.find(handle);
  if (shown == found->second.shown.end()) {
    return false;
  }
  row = shown->second;
  return true;
}

bool test_take_first() {
  void* player = inventory::local_player_inventory();
  auto move = reinterpret_cast<MoveItem>(natives.move_item);
  for (const auto& [horse, held] : bags) {
    const auto items = held.inventory ? inventory::items_in(held.inventory) : std::vector<void*>{};
    if (!items.empty() && player && move) {
      move(player, items.front(), 1, 0);
      return true;
    }
  }
  return false;
}

}  // namespace kcds::runtime::riding::saddlebags
