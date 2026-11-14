#include "stock_projection.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../appearance/grime.hpp"

namespace kcds::runtime::inventory {
namespace {

namespace integration = kcd2s::integration;

// Services + 224 holds the item services, whose + 232 is the inventory list (as Book's).
constexpr std::size_t kServicesItems = 224;
constexpr std::size_t kItemsInventories = 232;
// An inventory's items, behind their iterator factory.
constexpr std::size_t kInventoryItems = 232;
constexpr std::size_t kMakeIterator = 4;
constexpr std::size_t kIteratorNext = 1;
constexpr std::size_t kIteratorRelease = 0;
// An item.
constexpr std::size_t kItemHandle = 0x30;
// The item's class object; its GUID is at + 8. (+ 0x38 is the item's own GUID.)
constexpr std::size_t kItemClass = 0x48;
constexpr std::size_t kClassGuid = 8;
constexpr std::size_t kItemAmount = 0x50;
constexpr std::size_t kItemHealth = 0x54;
// ItemDesc + 144 is the quality the item is made in.
constexpr std::size_t kDescQuality = 144;
// Inventory change reasons: 4 is the script CreateItem's, 6 a removal by script.
constexpr unsigned kAddReason = 4;
constexpr unsigned kRemoveReason = 6;
// The server keeps health as a double of the float the game had; a float of it differs by rounding.
constexpr float kHealthTolerance = 1e-4f;
// Health moves by itself (food spoils): a change smaller than this is not reported, and an item
// kept for a row still shows it while its health is within this of the row's.
constexpr float kWearStep = 0.01f;
// Dirt and blood grow a little at a time: a piece is reported soiled once one of its bytes moved
// this far from what the server has (about 3 %), or went to or from nothing (a wash).
constexpr int kGrimeStep = 8;

using InventoryFind = void*(__fastcall*)(void* inventories, void* entity);
using ItemDescInit = void*(__fastcall*)(void* desc, const void* guid, int amount, float health);
using ItemDescFree = void(__fastcall*)(void* desc);
using InventoryAdd = void*(__fastcall*)(void* inventory, void* desc, unsigned reason, char flag);
using InventoryRemove = void*(__fastcall*)(void* inventory, void* item, unsigned reason,
                                           unsigned amount);
using ItemQuality = unsigned(__fastcall*)(void* item);

Natives natives;
Logger log_line = nullptr;
bool installed = false;

struct NativeItem {
  void* item = nullptr;
  std::uint64_t handle = 0;
  integration::Guid item_class{};
  std::uint32_t amount = 0;
  float health = 0;
  std::uint8_t quality = 1;
  integration::ItemGrime grime{};
};

// Whether the game's dirt and blood of a piece moved from what the server has.
bool grime_moved(const integration::ItemGrime& now, const integration::ItemGrime& known) {
  for (std::size_t i = 0; i < now.size(); ++i) {
    const int step = std::abs(int(now[i]) - int(known[i]));
    if (step >= kGrimeStep || (step > 0 && (now[i] == 0 || known[i] == 0))) {
      return true;
    }
  }
  return false;
}

// An item kept for a row, as the last stock or scan left it.
struct Kept {
  std::string row;
  NativeItem item;
};

// The items kept for the rows (by handle), the items nobody keeps that the last stock or scan saw,
// and whether a stock was shown on this connection yet.
std::unordered_map<std::uint64_t, Kept> kept;
std::unordered_set<std::uint64_t> unkept;
bool shown_once = false;
// The revision of the stock the game shows.
std::uint64_t shown_revision = 0;
// Per row: the units worn as the server last had them (from its stock), and as last reported.
std::unordered_map<std::string, std::uint32_t> stock_equipped;
std::unordered_map<std::string, std::uint32_t> reported_equipped;
// Weapon rows the last stock with quickslots puts into a weapon quickslot. A weapon on the belt is
// worn, and the game's quickslot assignment puts it there (quickslots.cpp); dressed through the
// equipment as well, each one would take the one before out of its slot. So
// these rows are not dressed here until a later stock changes what they wear.
std::unordered_set<std::string> slotted_rows;

template <class Function>
Function slot(void* object, std::size_t index) {
  return (*static_cast<Function**>(object))[index];
}

template <class Value>
Value at(const void* object, std::size_t offset) {
  Value value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

void* inventory_of(void* entity) {
  void* services = natives.services ? natives.services() : nullptr;
  void* items = services ? at<void*>(services, kServicesItems) : nullptr;
  void* inventories = items ? at<void*>(items, kItemsInventories) : nullptr;
  if (!inventories || !entity) {
    return nullptr;
  }
  return reinterpret_cast<InventoryFind>(natives.inventory_find)(inventories, entity);
}

void* local_inventory() {
  void* services = natives.services ? natives.services() : nullptr;
  void* entity = natives.local_entity ? natives.local_entity() : nullptr;
  void* items = services ? at<void*>(services, kServicesItems) : nullptr;
  void* inventories = items ? at<void*>(items, kItemsInventories) : nullptr;
  if (!inventories || !entity) {
    return nullptr;
  }
  return reinterpret_cast<InventoryFind>(natives.inventory_find)(inventories, entity);
}

NativeItem read_item(void* item) {
  NativeItem native;
  native.item = item;
  native.handle = at<std::uint64_t>(item, kItemHandle);
  if (void* item_class = at<void*>(item, kItemClass)) {
    native.item_class = at<integration::Guid>(item_class, kClassGuid);
  }
  native.amount = at<std::uint32_t>(item, kItemAmount);
  native.health = at<float>(item, kItemHealth);
  const unsigned quality = reinterpret_cast<ItemQuality>(natives.item_quality)(item);
  native.quality = std::uint8_t(quality == 0 || quality > 255 ? 1 : quality);
  // A piece of clothing's dirt and blood; zeros for everything else.
  appearance::grime::read_item(item, native.grime);
  return native;
}

// Items the player wears for a while outside their inventory (Player.wearItem), by
// handle: no stock keeps or takes them and no scan reports them.
std::unordered_set<std::uint64_t> worn_only;

// The inventory's items in its own order, the worn-only ones left out.
std::vector<NativeItem> read_items(void* inventory) {
  std::vector<NativeItem> items;
  void* container = static_cast<std::uint8_t*>(inventory) + kInventoryItems;
  void* iterator = slot<void*(__fastcall*)(void*)>(container, kMakeIterator)(container);
  if (!iterator) {
    return items;
  }
  while (void** entry = slot<void**(__fastcall*)(void*)>(iterator, kIteratorNext)(iterator)) {
    if (*entry) {
      NativeItem native = read_item(*entry);
      if (!worn_only.count(native.handle)) {
        items.push_back(native);
      }
    }
  }
  slot<void(__fastcall*)(void*)>(iterator, kIteratorRelease)(iterator);
  return items;
}

bool same_health(float a, float b) {
  return std::fabs(a - b) <= kHealthTolerance;
}

bool matches(const NativeItem& native, const integration::InventoryRowState& row) {
  return native.item_class == row.item && native.quality == row.quality &&
         same_health(native.health, row.health);
}

// Makes `amount` units of a row as one item; the game makes one unit of a class whose units do
// not stack. Null when it made nothing.
void* make_item(void* inventory, const integration::InventoryRowState& row, std::uint32_t amount) {
  alignas(16) std::uint8_t desc[512] = {};
  // The game reads the GUID with an aligned 16-byte load (appearance/outfit.cpp aligns it too).
  alignas(16) integration::Guid guid = row.item;
  reinterpret_cast<ItemDescInit>(natives.item_desc_init)(desc, guid.data(), int(amount),
                                                         row.health);
  const std::uint32_t quality = row.quality;
  std::memcpy(desc + kDescQuality, &quality, sizeof(quality));
  void* item =
      reinterpret_cast<InventoryAdd>(natives.inventory_add)(inventory, desc, kAddReason, 0);
  reinterpret_cast<ItemDescFree>(natives.item_desc_free)(desc);
  return item;
}

void take_units(void* inventory, const NativeItem& native, std::uint32_t amount) {
  reinterpret_cast<InventoryRemove>(natives.inventory_remove)(inventory, native.item, kRemoveReason,
                                                              amount >= native.amount ? 0 : amount);
}

// One row being brought to its amount: the items kept for it and the units they hold.
struct RowItems {
  std::vector<std::uint64_t> handles;
  std::uint32_t units = 0;
};

// Takes units over the row's amount from its last items.
unsigned trim(void* inventory, const integration::InventoryRowState& row, RowItems& mine,
              std::unordered_map<std::uint64_t, NativeItem>& known) {
  unsigned taken = 0;
  while (mine.units > row.amount && !mine.handles.empty()) {
    NativeItem& native = known[mine.handles.back()];
    const std::uint32_t excess = mine.units - row.amount;
    take_units(inventory, native, excess);
    ++taken;
    if (excess >= native.amount) {
      mine.units -= native.amount;
      known.erase(mine.handles.back());
      mine.handles.pop_back();
    } else {
      native.amount -= excess;
      mine.units -= excess;
    }
  }
  return taken;
}

// Makes the units the row lacks. The game makes one unit at a time of a class whose units do not
// stack, and may add units of one that stack to an item already there: what an item gained is
// what counts.
unsigned fill(void* inventory, const integration::InventoryRowState& row, RowItems& mine,
              std::unordered_map<std::uint64_t, NativeItem>& known,
              std::unordered_set<std::uint64_t>& claimed) {
  unsigned made = 0;
  while (mine.units < row.amount && made < row.amount) {
    void* item = make_item(inventory, row, row.amount - mine.units);
    if (!item) {
      if (log_line) {
        log_line("INVENTORY could not make row %s (%u of %u units shown)", row.id.c_str(),
                 mine.units, row.amount);
      }
      break;
    }
    const NativeItem native = read_item(item);
    const auto before = known.find(native.handle);
    const std::uint32_t gained =
        before == known.end() ? native.amount : native.amount - before->second.amount;
    known[native.handle] = native;
    if (gained == 0) {
      break;
    }
    if (!claimed.count(native.handle)) {
      claimed.insert(native.handle);
      mine.handles.push_back(native.handle);
      mine.units += native.amount;
    } else {
      mine.units += gained;
    }
    ++made;
  }
  return made;
}

std::unordered_set<void*> worn_now() {
  void* actor = natives.local_actor ? natives.local_actor() : nullptr;
  if (!actor || !natives.worn) {
    return {};
  }
  const std::vector<void*> items = natives.worn(actor);
  return {items.begin(), items.end()};
}

// Puts on or takes off items of a row until the body wears `wanted` of its units. True when the
// body still wears fewer.
bool dress(const RowItems& mine, std::uint32_t wanted,
           std::unordered_map<std::uint64_t, NativeItem>& known,
           const std::unordered_set<void*>& worn) {
  void* actor = natives.local_actor ? natives.local_actor() : nullptr;
  if (!actor || !natives.wear) {
    return false;
  }
  std::vector<const NativeItem*> on;
  std::vector<const NativeItem*> off;
  std::uint32_t wearing = 0;
  for (const std::uint64_t handle : mine.handles) {
    const NativeItem& native = known[handle];
    if (worn.count(native.item)) {
      on.push_back(&native);
      wearing += native.amount;
    } else {
      off.push_back(&native);
    }
  }
  while (wearing < wanted && !off.empty()) {
    void* item = off.back()->item;
    const int as_player = natives.equip_weapon ? natives.equip_weapon(actor, item) : -1;
    if (as_player >= 0 ? as_player == 1 : natives.wear(actor, item, true)) {
      wearing += off.back()->amount;
    } else if (log_line) {
      log_line("INVENTORY the game would not put item %p on (%u of %u worn)", off.back()->item,
               wearing, wanted);
    }
    off.pop_back();
  }
  while (wearing > wanted && !on.empty()) {
    if (natives.wear(actor, on.back()->item, false)) {
      wearing -= on.back()->amount;
    }
    on.pop_back();
  }
  return wearing < wanted;
}

// The body wears one stack of ammunition: putting on another stack of the same arrows takes the
// first off. Units of a row the body wants worn that lie in other items (an
// arrow picked up) join the stack it wears instead: they are taken and made again, and the game
// adds them to that stack. Only a row whose worn item holds more than one unit is a stack.
void fold_into_worn(void* inventory, const integration::InventoryRowState& row, RowItems& mine,
                    std::unordered_map<std::uint64_t, NativeItem>& known,
                    const std::unordered_set<void*>& worn) {
  if (row.equipped == 0 || mine.handles.size() < 2) {
    return;
  }
  std::uint64_t stack = 0;
  for (const std::uint64_t handle : mine.handles) {
    if (worn.count(known[handle].item) && known[handle].amount > 1) {
      stack = handle;
    }
  }
  if (!stack) {
    return;
  }
  std::vector<std::uint64_t> kept_handles;
  for (const std::uint64_t handle : mine.handles) {
    if (handle == stack || worn.count(known[handle].item)) {
      kept_handles.push_back(handle);
      continue;
    }
    const NativeItem loose = known[handle];
    take_units(inventory, loose, loose.amount);
    known.erase(handle);
    void* item = make_item(inventory, row, loose.amount);
    if (item) {
      const NativeItem joined = read_item(item);
      known[joined.handle] = joined;
      if (joined.handle != stack && std::find(kept_handles.begin(), kept_handles.end(),
                                              joined.handle) == kept_handles.end()) {
        kept_handles.push_back(joined.handle);
      }
    }
  }
  mine.handles = kept_handles;
}

// Rows the body could not wear all of when the server asked: putting on a bow takes its
// ammunition off, and ammunition goes on only with the weapon that shoots it. Rows
// are dressed again once every row is, and a row still short is dressed again on each scan for a
// short while, what its body wears not reported meanwhile.
constexpr std::chrono::milliseconds kDressRetry{2000};
struct PendingDress {
  std::uint32_t wanted = 0;
  std::chrono::steady_clock::time_point until;
};
std::unordered_map<std::string, PendingDress> pending_dress;

// What the inventory holds now becomes what the next scan compares with.
void remember(void* inventory) {
  std::unordered_map<std::uint64_t, Kept> next;
  unkept.clear();
  for (const NativeItem& native : read_items(inventory)) {
    const auto found = kept.find(native.handle);
    if (found != kept.end()) {
      next[native.handle] = {found->second.row, native};
    } else {
      unkept.insert(native.handle);
    }
  }
  kept = std::move(next);
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  installed = natives.services && natives.local_entity && natives.inventory_find &&
              natives.item_desc_init && natives.item_desc_free && natives.inventory_add &&
              natives.inventory_remove && natives.item_quality;
  return installed;
}

bool fill_pockets(void* entity, void* actor,
                  const std::vector<integration::InventoryRowState>& rows) {
  void* inventory = installed ? inventory_of(entity) : nullptr;
  if (!inventory) {
    return false;
  }
  std::unordered_set<void*> worn;
  if (actor && natives.worn) {
    for (void* item : natives.worn(actor)) {
      worn.insert(item);
    }
  }
  for (const NativeItem& native : read_items(inventory)) {
    if (worn.count(native.item) == 0) {
      take_units(inventory, native, native.amount);
    }
  }
  for (const auto& row : rows) {
    std::uint32_t left = row.amount;
    // A class whose units do not stack is made one item per unit.
    for (unsigned made = 0; left > 0 && made < 64; ++made) {
      void* item = make_item(inventory, row, left);
      if (!item) {
        break;
      }
      const std::uint32_t units = std::max<std::uint32_t>(1, at<std::uint32_t>(item, kItemAmount));
      left -= std::min(left, units);
    }
  }
  if (log_line) {
    log_line("PICKPOCKET victim's pockets hold %zu rows", rows.size());
  }
  return true;
}

std::vector<integration::InventoryRowState> census() {
  const std::unordered_set<void*> worn = worn_now();
  std::vector<integration::InventoryRowState> rows;
  void* inventory = installed ? local_inventory() : nullptr;
  if (!inventory) {
    return rows;
  }
  for (const NativeItem& native : read_items(inventory)) {
    if (native.amount == 0 || !(native.health >= 0.0f && native.health <= 1.0f)) {
      continue;
    }
    integration::InventoryRowState row;
    row.item = native.item_class;
    row.amount = native.amount;
    row.quality = native.quality;
    row.health = native.health;
    row.equipped = worn.count(native.item) ? native.amount : 0;
    row.grime = native.grime;
    rows.push_back(row);
    if (rows.size() >= integration::kMaxCensusRows) {
      break;
    }
  }
  return rows;
}

bool show(const integration::InventoryShow& stock) {
  void* inventory = installed ? local_inventory() : nullptr;
  if (!inventory) {
    return false;
  }
  std::unordered_map<std::uint64_t, NativeItem> known;
  std::vector<std::uint64_t> order;
  for (const NativeItem& native : read_items(inventory)) {
    known[native.handle] = native;
    order.push_back(native.handle);
  }
  std::unordered_set<std::uint64_t> claimed;
  std::vector<RowItems> rows(stock.rows.size());
  // First every row takes back the items kept for it that still show it.
  std::unordered_map<std::string, std::size_t> row_index;
  for (std::size_t index = 0; index < stock.rows.size(); ++index) {
    row_index.emplace(stock.rows[index].id, index);
  }
  for (const std::uint64_t handle : order) {
    const auto previous = kept.find(handle);
    if (previous == kept.end()) {
      continue;
    }
    const auto index = row_index.find(previous->second.row);
    if (index == row_index.end()) {
      continue;
    }
    const NativeItem& native = known[handle];
    const integration::InventoryRowState& row = stock.rows[index->second];
    if (native.item_class != row.item || native.quality != row.quality ||
        std::fabs(native.health - row.health) >= kWearStep) {
      continue;
    }
    claimed.insert(handle);
    rows[index->second].handles.push_back(handle);
    rows[index->second].units += known[handle].amount;
  }
  // Then rows short of units take items of their class and properties nobody claimed: the
  // census's, the save's, or one the game changed into this row's properties.
  for (std::size_t index = 0; index < stock.rows.size(); ++index) {
    const integration::InventoryRowState& row = stock.rows[index];
    for (const std::uint64_t handle : order) {
      if (rows[index].units >= row.amount) {
        break;
      }
      if (!claimed.count(handle) && matches(known[handle], row)) {
        claimed.insert(handle);
        rows[index].handles.push_back(handle);
        rows[index].units += known[handle].amount;
      }
    }
  }
  // Then rows still short take items of their class and quality nobody claimed, and the item takes
  // the row's condition where it is: the server put it otherwise (a wear rate), and an
  // item in the hands stays in them.
  for (std::size_t index = 0; index < stock.rows.size(); ++index) {
    const integration::InventoryRowState& row = stock.rows[index];
    for (const std::uint64_t handle : order) {
      if (rows[index].units >= row.amount) {
        break;
      }
      NativeItem& native = known[handle];
      if (claimed.count(handle) || native.item_class != row.item || native.quality != row.quality ||
          rows[index].units + native.amount > row.amount) {
        continue;
      }
      std::memcpy(static_cast<std::uint8_t*>(native.item) + kItemHealth, &row.health,
                  sizeof(row.health));
      native.health = row.health;
      claimed.insert(handle);
      rows[index].handles.push_back(handle);
      rows[index].units += native.amount;
    }
  }
  unsigned made = 0;
  unsigned taken = 0;
  unsigned grimed = 0;
  std::unordered_map<std::uint64_t, Kept> next;
  // Rows that got new items: those are not on the body yet, whatever the count says (a row that
  // keeps its id but changes its class, live).
  std::vector<bool> fresh(stock.rows.size(), false);
  for (std::size_t index = 0; index < stock.rows.size(); ++index) {
    const integration::InventoryRowState& row = stock.rows[index];
    taken += trim(inventory, row, rows[index], known);
    const unsigned row_made = fill(inventory, row, rows[index], known, claimed);
    fresh[index] = row_made > 0;
    made += row_made;
    taken += trim(inventory, row, rows[index], known);
    fold_into_worn(inventory, row, rows[index], known, worn_now());
    for (const std::uint64_t handle : rows[index].handles) {
      // The row's dirt and blood go on its pieces (a script washed or soiled them, or they came
      // from a chest or another player).
      NativeItem& native = known[handle];
      if (native.grime != row.grime && appearance::grime::write_item(native.item, row.grime)) {
        native.grime = row.grime;
        ++grimed;
      }
      next[handle] = {row.id, native};
    }
  }
  // The body wears what the server changed; a count it merely repeats leaves the body alone. Worn
  // weapons the stock puts into weapon quickslots go on through the quickslots instead.
  if (stock.quickslots) {
    slotted_rows.clear();
    for (std::size_t slot = 0; slot < integration::kQuickslots; ++slot) {
      for (const std::string& id :
           {stock.quickslots->primary[slot], stock.quickslots->secondary[slot]}) {
        if (!id.empty()) {
          slotted_rows.insert(id);
        }
      }
    }
  }
  const std::unordered_set<void*> worn = worn_now();
  std::unordered_map<std::string, std::uint32_t> next_equipped;
  std::vector<std::size_t> dressed;
  for (std::size_t index = 0; index < stock.rows.size(); ++index) {
    const integration::InventoryRowState& row = stock.rows[index];
    const auto previous = stock_equipped.find(row.id);
    const std::uint32_t before = previous == stock_equipped.end() ? 0 : previous->second;
    if (slotted_rows.count(row.id)) {
      if (row.equipped > 0 && (stock.quickslots || row.equipped == before)) {
        next_equipped[row.id] = row.equipped;
        pending_dress.erase(row.id);
        continue;
      }
      // A later stock changed what the row wears: it is dressed as any other again.
      slotted_rows.erase(row.id);
    }
    if (row.equipped != before || (fresh[index] && row.equipped > 0)) {
      dress(rows[index], row.equipped, known, worn);
      dressed.push_back(index);
    }
    next_equipped[row.id] = row.equipped;
  }
  const std::unordered_set<void*> worn_after = worn_now();
  for (const std::size_t index : dressed) {
    const integration::InventoryRowState& row = stock.rows[index];
    if (dress(rows[index], row.equipped, known, worn_after)) {
      pending_dress[row.id] = {row.equipped, std::chrono::steady_clock::now() + kDressRetry};
    } else {
      pending_dress.erase(row.id);
    }
  }
  stock_equipped = next_equipped;
  reported_equipped = std::move(next_equipped);
  // Items kept for rows that no longer hold them go; on the first stock of a connection a restore
  // also takes everything else it does not hold.
  const bool first = !shown_once;
  for (const std::uint64_t handle : order) {
    if (claimed.count(handle)) {
      continue;
    }
    if ((first && stock.replace) || kept.count(handle)) {
      take_units(inventory, known[handle], known[handle].amount);
      ++taken;
    }
  }
  kept = std::move(next);
  shown_once = true;
  shown_revision = stock.revision;
  remember(inventory);
  if (first && !stock.replace) {
    // A stock that grew from the census: what the game holds beyond it came after the census
    // (a character creator's clothes), and the next scan reports it as gained.
    unkept.clear();
  }
  if (log_line) {
    log_line(
        "INVENTORY shows revision %llu: %zu rows, %u items made, %u taken, %u soiled or washed",
        static_cast<unsigned long long>(stock.revision), stock.rows.size(), made, taken, grimed);
  }
  return true;
}

std::vector<integration::InventoryNativeChange> scan() {
  using Kind = integration::InventoryNativeChange::Kind;
  std::vector<integration::InventoryNativeChange> changes;
  void* inventory = installed && shown_once ? local_inventory() : nullptr;
  if (!inventory) {
    return changes;
  }
  std::unordered_map<std::uint64_t, NativeItem> now;
  for (const NativeItem& native : read_items(inventory)) {
    now[native.handle] = native;
  }
  auto change = [&](Kind kind, const std::string& row, const NativeItem& native,
                    std::uint32_t amount) {
    integration::InventoryNativeChange entry;
    entry.kind = kind;
    entry.id = row;
    entry.item = native.item_class;
    entry.amount = amount;
    entry.quality = native.quality;
    entry.health = std::fmin(1.0f, std::fmax(0.0f, native.health));
    entry.grime = native.grime;
    changes.push_back(entry);
  };
  for (auto it = kept.begin(); it != kept.end();) {
    const Kept& before = it->second;
    const auto found = now.find(it->first);
    if (found == now.end()) {
      change(Kind::Lost, before.row, before.item, before.item.amount);
      it = kept.erase(it);
      continue;
    }
    const NativeItem& after = found->second;
    if (after.amount < before.item.amount) {
      change(Kind::Lost, before.row, before.item, before.item.amount - after.amount);
    } else if (after.amount > before.item.amount) {
      change(Kind::Gained, before.row, after, after.amount - before.item.amount);
    }
    const float reported_health = before.item.health;
    const bool worn = after.quality != before.item.quality ||
                      std::fabs(after.health - reported_health) >= kWearStep;
    if (after.amount > 0 && worn) {
      change(Kind::Worn, before.row, after, std::min(after.amount, before.item.amount));
    }
    const integration::ItemGrime reported_grime = before.item.grime;
    const bool soiled = grime_moved(after.grime, reported_grime);
    if (after.amount > 0 && soiled) {
      change(Kind::Soiled, before.row, after, std::min(after.amount, before.item.amount));
    }
    it->second.item = after;
    if (!worn) {
      // The health the server knows stays the one compared with.
      it->second.item.health = reported_health;
    }
    if (!soiled) {
      // And so do its dirt and blood.
      it->second.item.grime = reported_grime;
    }
    ++it;
  }
  for (const auto& [handle, native] : now) {
    if (!kept.count(handle) && !unkept.count(handle) && native.amount > 0) {
      change(Kind::Gained, std::string(), native, native.amount);
      unkept.insert(handle);
    }
  }
  // Rows the game refused to dress a moment ago are dressed again.
  const auto tick = std::chrono::steady_clock::now();
  for (auto it = pending_dress.begin(); it != pending_dress.end();) {
    RowItems mine;
    for (const auto& [handle, entry] : kept) {
      if (entry.row == it->first && now.count(handle)) {
        mine.handles.push_back(handle);
      }
    }
    if (tick > it->second.until || !dress(mine, it->second.wanted, now, worn_now())) {
      it = pending_dress.erase(it);
    } else {
      ++it;
    }
  }
  // How many units of each row the body wears.
  const std::unordered_set<void*> worn = worn_now();
  std::unordered_map<std::string, std::uint32_t> wearing;
  for (const auto& [handle, entry] : kept) {
    std::uint32_t& units = wearing[entry.row];
    const auto found = now.find(handle);
    if (found != now.end() && worn.count(found->second.item)) {
      units += found->second.amount;
    }
  }
  for (auto& [row, reported] : reported_equipped) {
    if (pending_dress.count(row)) {
      continue;
    }
    const auto found = wearing.find(row);
    const std::uint32_t units = found == wearing.end() ? 0 : found->second;
    if (units != reported) {
      NativeItem none;
      change(Kind::Equipped, row, none, units);
      reported = units;
    }
  }
  for (auto it = unkept.begin(); it != unkept.end();) {
    it = now.count(*it) ? std::next(it) : unkept.erase(it);
  }
  return changes;
}

void settle_worn(bool quickslotted, bool new_skeleton) {
  void* inventory = installed && shown_once ? local_inventory() : nullptr;
  if (!inventory) {
    return;
  }
  void* actor = natives.local_actor ? natives.local_actor() : nullptr;
  if (new_skeleton && actor && natives.is_weapon && natives.wear) {
    unsigned taken = 0;
    for (void* item : worn_now()) {
      if (natives.is_weapon(actor, item) && natives.wear(actor, item, false)) {
        ++taken;
      }
    }
    if (log_line && taken > 0) {
      log_line("INVENTORY %u weapons off the old skeleton, to go on the new one", taken);
    }
  }
  std::unordered_map<std::uint64_t, NativeItem> now;
  for (const NativeItem& native : read_items(inventory)) {
    now[native.handle] = native;
  }
  const std::unordered_set<void*> worn = worn_now();
  for (const auto& [row, wanted] : stock_equipped) {
    if (!quickslotted && slotted_rows.count(row)) {
      continue;
    }
    RowItems mine;
    for (const auto& [handle, entry] : kept) {
      if (entry.row == row && now.count(handle)) {
        mine.handles.push_back(handle);
      }
    }
    if (dress(mine, wanted, now, worn)) {
      pending_dress[row] = {wanted, std::chrono::steady_clock::now() + kDressRetry};
    }
  }
}

bool row_of(std::uint64_t handle, std::string& row, std::uint64_t& revision) {
  const auto found = kept.find(handle);
  if (found == kept.end()) {
    return false;
  }
  row = found->second.row;
  revision = shown_revision;
  return true;
}

std::vector<void*> items_of(const std::string& row) {
  std::vector<void*> items;
  void* inventory = local_inventory();
  if (!installed || !inventory) {
    return items;
  }
  for (const NativeItem& native : read_items(inventory)) {
    const auto found = kept.find(native.handle);
    if (found != kept.end() && found->second.row == row) {
      items.push_back(native.item);
    }
  }
  return items;
}

std::unordered_map<std::uint64_t, std::string> fill_container(
    void* inventory, const std::vector<integration::InventoryRowState>& rows, void* wearer) {
  std::unordered_map<std::uint64_t, std::string> shown;
  if (!installed || !inventory) {
    return shown;
  }
  std::unordered_set<void*> worn;
  if (wearer && natives.worn) {
    const std::vector<void*> items = natives.worn(wearer);
    worn.insert(items.begin(), items.end());
  }
  // The wearer's worn items, which show the rows it wears (a dead NPC's outfit rows);
  // each is used for one row at most.
  std::vector<NativeItem> worn_items;
  for (const NativeItem& native : read_items(inventory)) {
    if (worn.count(native.item)) {
      worn_items.push_back(native);
    } else {
      take_units(inventory, native, native.amount);
    }
  }
  for (const integration::InventoryRowState& row : rows) {
    // Units of the row the wearer wears are shown by its worn items of that class.
    std::uint32_t made = 0;
    for (auto it = worn_items.begin(); it != worn_items.end() && made < row.equipped;) {
      if (it->item_class != row.item) {
        ++it;
        continue;
      }
      shown[it->handle] = row.id;
      made += it->amount;
      it = worn_items.erase(it);
    }
    // Units per item made for the row: the game makes one unit of a class that does not stack
    // per call, all of them at once otherwise.
    std::unordered_map<std::uint64_t, std::uint32_t> held;
    for (std::uint32_t attempt = 0; made < row.amount && attempt < row.amount; ++attempt) {
      void* item = make_item(inventory, row, row.amount - made);
      if (!item) {
        break;
      }
      const auto handle = at<std::uint64_t>(item, kItemHandle);
      const auto amount = at<std::uint32_t>(item, kItemAmount);
      if (amount <= held[handle]) {
        break;
      }
      made += amount - held[handle];
      held[handle] = amount;
      shown[handle] = row.id;
    }
  }
  return shown;
}

void* inventory_of_entity(void* entity) {
  return installed ? inventory_of(entity) : nullptr;
}

void* local_player_inventory() {
  return installed ? local_inventory() : nullptr;
}

std::vector<void*> items_in(void* inventory) {
  std::vector<void*> items;
  if (!installed || !inventory) {
    return items;
  }
  for (const NativeItem& native : read_items(inventory)) {
    items.push_back(native.item);
  }
  return items;
}

std::vector<std::pair<std::uint64_t, integration::Guid>> local_item_classes() {
  std::vector<std::pair<std::uint64_t, integration::Guid>> items;
  void* inventory = installed ? local_inventory() : nullptr;
  if (!inventory) {
    return items;
  }
  for (const NativeItem& native : read_items(inventory)) {
    items.emplace_back(native.handle, native.item_class);
  }
  return items;
}

void set_worn_only(void* item, bool worn_only_item) {
  if (!item) {
    return;
  }
  const std::uint64_t handle = read_item(item).handle;
  if (worn_only_item) {
    worn_only.insert(handle);
  } else {
    worn_only.erase(handle);
  }
}

bool take_unit(void* item) {
  void* inventory = local_inventory();
  if (!installed || !inventory || !item) {
    return false;
  }
  take_units(inventory, read_item(item), 1);
  return true;
}

void reset() {
  shown_revision = 0;
  kept.clear();
  unkept.clear();
  shown_once = false;
  stock_equipped.clear();
  slotted_rows.clear();
  pending_dress.clear();
  reported_equipped.clear();
}

}  // namespace kcds::runtime::inventory
