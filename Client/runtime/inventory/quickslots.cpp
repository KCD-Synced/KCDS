#include "quickslots.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "stock_projection.hpp"

namespace kcds::runtime::inventory {
namespace {

namespace integration = kcd2s::integration;

constexpr std::size_t kActorSoul = 1640;
constexpr std::size_t kSoulEquipment = 408;
constexpr std::size_t kEquipmentManager = 288;
constexpr std::size_t kManagerOwner = 8;
constexpr std::size_t kEquipmentOutfits = 272;
constexpr std::size_t kOutfitManager = 88;
constexpr std::size_t kCurrentOutfit = 1760;
constexpr std::size_t kOutfitBlocks = 136;
constexpr std::size_t kOutfitBlockSize = 544;
constexpr std::size_t kItemsManager = 256;
// The game keeps a handful of outfits; anything past this is not an outfit index.
constexpr std::uint8_t kMaxOutfits = 16;
// A quick access manager.
constexpr std::size_t kSlots = 48;
constexpr std::size_t kSlotSize = 48;
constexpr std::size_t kSlotSecondary = 8;
constexpr std::size_t kSelected = 240;
constexpr std::size_t kUsable = 244;
constexpr std::size_t kSelectSlot = 40 / 8;
constexpr std::size_t kAssignSlot = 176 / 8;
constexpr std::size_t kSlotUsable = 272 / 8;
// The manager an item belongs in, through the equipment manager's owner.
constexpr std::size_t kOwnerItems = 480 / 8;
constexpr std::size_t kItemsQuickAccess = 392 / 8;
constexpr std::size_t kQuickAccessFor = 144 / 8;
// An item's handle.
constexpr std::size_t kItemHandle = 0x30;

using Select = void*(__fastcall*)(void* manager, unsigned slot, char on);
using Assign = void*(__fastcall*)(void* manager, void* item, unsigned slot);
using Usable = bool(__fastcall*)(void* manager, unsigned slot);
using Getter = void*(__fastcall*)(void* object);
using ManagerFor = void*(__fastcall*)(void* object, void* item);

template <class Value>
Value at(const void* object, std::size_t offset) {
  Value value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

template <class Function>
Function slot(void* object, std::size_t index) {
  return (*static_cast<Function**>(object))[index];
}

struct Managers {
  void* weapons = nullptr;
  void* items = nullptr;
  void* equipment_manager = nullptr;
};

// The current outfit's two quick access managers; both null when they cannot be found.
Managers managers_of(void* actor) {
  Managers found;
  void* soul = actor ? at<void*>(actor, kActorSoul) : nullptr;
  if (!soul) {
    return found;
  }
  void* equipment = static_cast<std::uint8_t*>(soul) + kSoulEquipment;
  void* outfits_owner = at<void*>(equipment, kEquipmentOutfits);
  if (!outfits_owner) {
    return found;
  }
  auto* outfits = static_cast<std::uint8_t*>(outfits_owner) + kOutfitManager;
  const std::uint8_t outfit = at<std::uint8_t>(outfits, kCurrentOutfit);
  if (outfit >= kMaxOutfits) {
    return found;
  }
  std::uint8_t* block = outfits + kOutfitBlocks + kOutfitBlockSize * outfit;
  void* weapons = block;
  void* items = block + kItemsManager;
  // Both are quick access managers: the same vtable, a selection and a count within four.
  void* table = at<void*>(weapons, 0);
  if (!table || at<void*>(items, 0) != table) {
    return found;
  }
  for (void* manager : {weapons, items}) {
    if (at<std::uint32_t>(manager, kUsable) > integration::kQuickslots) {
      return found;
    }
  }
  found.weapons = weapons;
  found.items = items;
  found.equipment_manager = at<void*>(equipment, kEquipmentManager);
  return found;
}

void* slot_item(void* manager, std::size_t index, bool secondary) {
  return at<void*>(manager, kSlots + kSlotSize * index + (secondary ? kSlotSecondary : 0));
}

std::string row_of_item(void* item) {
  std::string row;
  std::uint64_t revision = 0;
  if (!item || !row_of(at<std::uint64_t>(item, kItemHandle), row, revision)) {
    return {};
  }
  return row;
}

std::uint8_t selected(void* manager) {
  const std::uint32_t value = at<std::uint32_t>(manager, kSelected);
  return std::uint8_t(value < integration::kQuickslots ? value : 0);
}

// The quick access manager the game puts an item in, or null.
void* manager_for(void* equipment_manager, void* item) {
  void* owner = equipment_manager ? at<void*>(equipment_manager, kManagerOwner) : nullptr;
  void* items = owner ? slot<Getter>(owner, kOwnerItems)(owner) : nullptr;
  void* quick_access = items ? slot<Getter>(items, kItemsQuickAccess)(items) : nullptr;
  return quick_access ? slot<ManagerFor>(quick_access, kQuickAccessFor)(quick_access, item)
                      : nullptr;
}

// One of the row's items for a slot: one no other slot took if there is one.
void* item_for(const std::string& row, std::unordered_set<void*>& taken) {
  const std::vector<void*> items = items_of(row);
  for (void* item : items) {
    if (!taken.count(item)) {
      taken.insert(item);
      return item;
    }
  }
  return items.empty() ? nullptr : items.front();
}

void put(void* manager, const Managers& managers, unsigned index, const std::string& row,
         std::unordered_set<void*>& taken, Logger log_line, unsigned& unusable) {
  if (row.empty()) {
    return;
  }
  if (!slot<Usable>(manager, kSlotUsable)(manager, index)) {
    // The outfit has no such slot (belts and sheaths add them).
    ++unusable;
    if (log_line) {
      log_line("INVENTORY quickslot %u row %s: the outfit cannot use the slot", index, row.c_str());
    }
    return;
  }
  void* item = item_for(row, taken);
  if (!item) {
    return;
  }
  void* owner = manager_for(managers.equipment_manager, item);
  if (owner != manager) {
    // The game keeps this item in another quick access list (or none): it is not put here.
    if (log_line) {
      log_line("INVENTORY quickslot %u row %s belongs to manager %p, not %p", index, row.c_str(),
               owner, manager);
    }
    return;
  }
  if (slot_item(manager, index, false) == item || slot_item(manager, index, true) == item) {
    return;
  }
  slot<Assign>(manager, kAssignSlot)(manager, item, index);
}

}  // namespace

bool read_quickslots(void* actor, integration::InventoryQuickslots& slots) {
  const Managers managers = managers_of(actor);
  if (!managers.weapons) {
    return false;
  }
  for (std::size_t index = 0; index < integration::kQuickslots; ++index) {
    slots.primary[index] = row_of_item(slot_item(managers.weapons, index, false));
    slots.secondary[index] = row_of_item(slot_item(managers.weapons, index, true));
    slots.items[index] = row_of_item(slot_item(managers.items, index, false));
  }
  slots.active_weapon = selected(managers.weapons);
  slots.active_item = selected(managers.items);
  return true;
}

bool restore_quickslots(void* actor, const integration::InventoryQuickslots& slots, Logger log_line,
                        unsigned& unusable) {
  unusable = 0;
  const Managers managers = managers_of(actor);
  if (!managers.weapons || !managers.equipment_manager) {
    return false;
  }
  std::unordered_set<void*> taken;
  for (unsigned index = 0; index < integration::kQuickslots; ++index) {
    put(managers.weapons, managers, index, slots.primary[index], taken, log_line, unusable);
    put(managers.weapons, managers, index, slots.secondary[index], taken, log_line, unusable);
    put(managers.items, managers, index, slots.items[index], taken, log_line, unusable);
  }
  for (const auto& [manager, wanted] :
       {std::pair{managers.weapons, slots.active_weapon}, {managers.items, slots.active_item}}) {
    if (selected(manager) != wanted && slot<Usable>(manager, kSlotUsable)(manager, wanted)) {
      slot<Select>(manager, kSelectSlot)(manager, wanted, 1);
    }
  }
  return true;
}

}  // namespace kcds::runtime::inventory
