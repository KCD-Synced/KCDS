#include "inventory_use.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"

#include "../core/game_layouts.hpp"
#include "kcd2s/integration/guid.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "item_types.hpp"
#include "stock_projection.hpp"

namespace kcds::runtime::inventory {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

using Consume = void*(__fastcall*)(void* controller, void* item, char skip_confirmation);
using Learn = void*(__fastcall*)(void* controller, void* item);
using Primary = void*(__fastcall*)(void** controller, void*** item);
using EquipToggle = void*(__fastcall*)(void* controller, void* id, void* slot, void* target);
using PrimaryState = std::uint8_t*(__fastcall*)(void* controller, std::uint8_t* state, void* item);
using StringCopy = void**(__fastcall*)(void** copy, const void* text);
using StringAssign = void*(__fastcall*)(void* text, const char* value);
// The screen controller's item by its id: takes the id string by value and releases it.
using ItemById = void*(__fastcall*)(void* controller, void** id);
using ItemOwner = void*(__fastcall*)(void* item);
// The owner's use of an item (vtable + 1560): result flags (0x1FE: why it failed), the item, an
// amount and the consume path's other arguments.
using OwnerUse = void(__fastcall*)(void* owner, unsigned* result, void* item, int amount, int a,
                                   char b, int c);
using EatItemBind = std::uint64_t(__fastcall*)(void* bind, void* handler, const char* item);
using HandlerSoul = void*(__fastcall*)(void* handler);
using ScriptEndBool = std::uint64_t(__fastcall*)(void* handler, const char* value);
using SoulOwner = void*(__fastcall*)(void* soul, int which);
using ItemDescInit = void*(__fastcall*)(void* desc, const void* guid, int amount, float health);
using ItemDescFree = void(__fastcall*)(void* desc);
using InventoryAdd = void*(__fastcall*)(void* inventory, void* desc, unsigned reason, char flag);
using InventoryRemove = void*(__fastcall*)(void* inventory, void* item, unsigned reason,
                                           unsigned amount);
using InventoryOf = void*(__fastcall*)(void* equipment);

// An item: its handle (the native UID) and its class object, whose GUID is at + 8.
constexpr std::size_t kItemHandle = 0x30;
constexpr std::size_t kItemClass = 0x48;
constexpr std::size_t kClassGuid = 8;
// The screen controller's item lookup (vtable byte offset).
constexpr std::size_t kControllerItemById = 200;
// The primary action's state: available, enabled, the reason it is not and its hint label.
constexpr std::size_t kStateAvailable = 0;
constexpr std::size_t kStateEnabled = 1;
constexpr std::size_t kStateReason = 8;
constexpr std::size_t kStateLabel = 16;
constexpr std::size_t kOwnerUse = 1560 / 8;
constexpr unsigned kUseFailed = 0x1FE;
// What a use the server decides answers at once: failed, nothing used.
constexpr unsigned kUseAskedServer = 0x2;
// Food and potions share one class type, the apple's.
constexpr char kFoodClass[] = "2264f217-590e-4c0f-a4c6-f50c6532b9f6";
// A use waits this long for the server's verdict, and at most this many at once.
constexpr std::chrono::seconds kVerdictWait{10};
constexpr std::size_t kMaxWaiting = 16;
constexpr std::size_t kMaxQueuedUses = 16;

Logger log_line = nullptr;
Consume original_consume = nullptr;
Learn original_learn = nullptr;
Primary original_primary = nullptr;
EquipToggle original_equip_toggle = nullptr;
PrimaryState original_primary_state = nullptr;
StringCopy string_copy = nullptr;
StringAssign string_assign = nullptr;
ItemOwner item_owner = nullptr;
UseNatives use_natives;
EatItemBind original_eat_item = nullptr;
// The soul's equipment (its inventory's owner) and the inventory change reasons the game's own
// EatItem uses: 4 to add, 2 to take back a serving it could not use.
constexpr std::size_t kSoulEquipment = kcds::layouts::Soul::kEquipment;
constexpr unsigned kServingAddReason = 4;
constexpr unsigned kServingRemoveReason = 2;

struct Use {
  std::string item;
  std::uint64_t uid = 0;
  std::string row;
  std::uint64_t revision = 0;
  const char* action = "";
};

std::mutex overrides_mutex;
std::unordered_set<std::string> override_classes;
std::unordered_set<std::uint64_t> override_uids;

std::mutex uses_mutex;
std::vector<Use> uses;

// Uses waiting for the server's verdict, by request.
struct Waiting {
  std::uint64_t uid = 0;
  std::string row;
  std::chrono::steady_clock::time_point asked_at;
  bool pot = false;
  integration::Guid item{};
};
std::mutex consume_mutex;
std::map<std::uint32_t, Waiting> waiting;
std::vector<ConsumeRequest> consume_requests;
std::uint32_t next_request = 1;

template <class Value>
Value at(const void* object, std::size_t offset) {
  Value value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

// True when a resource overrides this item and it is a row of the server's stock; `use` then
// names it.
bool overridden(void* item, Use& use) {
  if (!item) {
    return false;
  }
  use.uid = at<std::uint64_t>(item, kItemHandle);
  void* item_class = at<void*>(item, kItemClass);
  if (!item_class) {
    return false;
  }
  use.item = integration::guid_text(at<integration::Guid>(item_class, kClassGuid));
  {
    std::lock_guard<std::mutex> lock(overrides_mutex);
    if (!override_uids.count(use.uid) && !override_classes.count(use.item)) {
      return false;
    }
  }
  return row_of(use.uid, use.row, use.revision);
}

// True when the action on this item is a resource's: the use waits for the next pump. Presses on
// one item in one frame count once.
bool take_over(void* item, const char* action) {
  Use use;
  if (!overridden(item, use)) {
    return false;
  }
  use.action = action;
  std::lock_guard<std::mutex> lock(uses_mutex);
  for (const Use& queued : uses) {
    if (queued.uid == use.uid) {
      return true;
    }
  }
  // The queue holds at most this many uses; a press beyond them is dropped (still the resource's).
  if (uses.size() < kMaxQueuedUses) {
    uses.push_back(std::move(use));
  }
  return true;
}

// The actor's own use, hooked; set while the client itself uses an item (a verdict, a serving),
// whose use the server decided already.
using HookedOwnerUse = unsigned*(__fastcall*)(void* owner, unsigned* result, void* item, int amount,
                                              int a, int b, int c);
HookedOwnerUse original_owner_use = nullptr;
thread_local bool own_use = false;

// Uses of usable registered types waiting to go to the server.
std::mutex custom_mutex;
std::vector<CustomUse> custom_uses;

// True when the item is of a usable type a resource registered: its use goes to the server (an
// item of one that no row keeps does nothing).
bool custom_use(void* item) {
  void* item_class = item ? at<void*>(item, kItemClass) : nullptr;
  std::string label;
  if (!item_class || !usable_type(item_class, label)) {
    return false;
  }
  CustomUse use;
  if (row_of(at<std::uint64_t>(item, kItemHandle), use.row, use.revision)) {
    std::lock_guard<std::mutex> lock(custom_mutex);
    if (custom_uses.size() < kMaxQueuedUses) {
      custom_uses.push_back(std::move(use));
    }
  }
  return true;
}

// Whether an item is food or a potion (its class is of the apple's class type).
bool is_food(void* item) {
  void* item_class = item ? at<void*>(item, kItemClass) : nullptr;
  if (!item_class) {
    return false;
  }
  static void* food_type = nullptr;
  if (!food_type) {
    integration::Guid apple{};
    if (integration::parse_guid(kFoodClass, apple)) {
      if (void* apple_class = game_item_class(apple)) {
        food_type = at<void*>(apple_class, 0);
      }
    }
  }
  return food_type && at<void*>(item_class, 0) == food_type;
}

// True when the server decides this use: the item is a row of the stock. A press on an item
// already waiting for its verdict is taken too.
bool ask_server(void* item) {
  if (!item || !item_owner) {
    return false;
  }
  const auto uid = at<std::uint64_t>(item, kItemHandle);
  std::string row;
  std::uint64_t revision = 0;
  if (!row_of(uid, row, revision)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(consume_mutex);
  const auto now = std::chrono::steady_clock::now();
  for (auto it = waiting.begin(); it != waiting.end();) {
    if (now - it->second.asked_at > kVerdictWait) {
      it = waiting.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& [request, pending] : waiting) {
    if (pending.uid == uid) {
      return true;
    }
  }
  if (waiting.size() >= kMaxWaiting) {
    return true;
  }
  const std::uint32_t request = next_request++;
  waiting[request] = Waiting{uid, row, now};
  consume_requests.push_back(ConsumeRequest{request, row, revision});
  return true;
}

// A serving from a pot or a barrel for the local soul: the server decides. The script hears
// "eaten" now, so the trigger goes on into the game's own animation (KettleActionTrigger:ReportUse
// returns before ActionTrigger's PlayAnimationAction when it does not); on the verdict
// the client eats it as the game's EatItem would, or nothing when the server refuses.
std::uint64_t __fastcall eat_item(void* bind, void* handler, const char* item) {
  void* soul = use_natives.handler_soul
                   ? reinterpret_cast<HandlerSoul>(use_natives.handler_soul)(handler)
                   : nullptr;
  void* local = use_natives.local_soul ? use_natives.local_soul() : nullptr;
  integration::Guid guid{};
  if (!soul || soul != local || !item || !integration::parse_guid(item, guid)) {
    return original_eat_item(bind, handler, item);
  }
  {
    std::lock_guard<std::mutex> lock(consume_mutex);
    if (waiting.size() < kMaxWaiting) {
      const std::uint32_t request = next_request++;
      Waiting pending;
      pending.asked_at = std::chrono::steady_clock::now();
      pending.pot = true;
      pending.item = guid;
      waiting[request] = pending;
      ConsumeRequest asked;
      asked.request = request;
      asked.pot = true;
      asked.item = guid;
      consume_requests.push_back(asked);
    }
  }
  if (log_line) {
    log_line("INVENTORY serving of %s asked of the server", item);
  }
  const char eaten = 1;
  return reinterpret_cast<ScriptEndBool>(use_natives.script_end_bool)(handler, &eaten);
}

// Eats one unit of a class as the game's EatItem does: made in the soul's inventory and used by
// its actor; taken back when the use fails. Whether it was used.
bool eat_serving(const integration::Guid& item) {
  void* soul = use_natives.local_soul ? use_natives.local_soul() : nullptr;
  if (!soul) {
    return false;
  }
  void* equipment = static_cast<std::uint8_t*>(soul) + kSoulEquipment;
  void* inventory = (*static_cast<InventoryOf**>(equipment))[0](equipment);
  void* owner = reinterpret_cast<SoulOwner>(use_natives.soul_owner)(soul, 0);
  if (!inventory || !owner) {
    return false;
  }
  alignas(16) std::uint8_t desc[512] = {};
  alignas(16) integration::Guid guid = item;
  reinterpret_cast<ItemDescInit>(use_natives.item_desc_init)(desc, guid.data(), 1, -1.0f);
  void* made = reinterpret_cast<InventoryAdd>(use_natives.inventory_add)(inventory, desc,
                                                                         kServingAddReason, 0);
  bool used = false;
  if (made) {
    unsigned result = 0;
    own_use = true;
    (*static_cast<OwnerUse**>(owner))[kOwnerUse](owner, &result, made, 1, -1, 1, -1);
    own_use = false;
    used = (result & kUseFailed) == 0;
    if (!used) {
      reinterpret_cast<InventoryRemove>(use_natives.inventory_remove)(inventory, made,
                                                                      kServingRemoveReason, 0);
    }
  }
  reinterpret_cast<ItemDescFree>(use_natives.item_desc_free)(desc);
  return used;
}

// Food or a potion of the server's rows the local player uses outside the inventory screen (a
// food quickslot): the server decides, as for the screen's eating; the game hears it failed.
unsigned* __fastcall owner_use(void* owner, unsigned* result, void* item, int amount, int a, int b,
                               int c) {
  if (!own_use && result && item && use_natives.local_soul && use_natives.soul_owner) {
    void* soul = use_natives.local_soul();
    void* local = soul ? reinterpret_cast<SoulOwner>(use_natives.soul_owner)(soul, 0) : nullptr;
    if (owner == local && is_food(item) && ask_server(item)) {
      *result = kUseAskedServer;
      if (log_line) {
        log_line("INVENTORY food used outside the inventory screen: asked the server");
      }
      return result;
    }
  }
  return original_owner_use(owner, result, item, amount, a, b, c);
}

void* item_by_id(void* controller, void* id) {
  if (!controller || !id || !string_copy) {
    return nullptr;
  }
  void* copy = nullptr;
  string_copy(&copy, id);
  const auto find = at<ItemById>(at<void*>(controller, 0), kControllerItemById);
  return find(controller, &copy);
}

void* __fastcall consume(void* controller, void* item, char skip_confirmation) {
  if (take_over(item, "secondary") || ask_server(item)) {
    return nullptr;
  }
  return original_consume(controller, item, skip_confirmation);
}

void* __fastcall learn(void* controller, void* item) {
  if (take_over(item, "secondary")) {
    return nullptr;
  }
  return original_learn(controller, item);
}

void* __fastcall primary(void** controller, void*** item) {
  void* chosen = item && *item ? **item : nullptr;
  if (take_over(chosen, "primary") || custom_use(chosen)) {
    return nullptr;
  }
  return original_primary(controller, item);
}

void* __fastcall equip_toggle(void* controller, void* id, void* slot, void* target) {
  void* chosen = item_by_id(controller, id);
  if (take_over(chosen, "activate") || custom_use(chosen)) {
    return nullptr;
  }
  return original_equip_toggle(controller, id, slot, target);
}

// An overridden item always offers its primary action, labelled Use when the game has none.
std::uint8_t* __fastcall primary_state(void* controller, std::uint8_t* state, void* item) {
  std::uint8_t* result = original_primary_state(controller, state, item);
  Use use;
  std::string custom_label;
  void* item_class = item ? at<void*>(item, kItemClass) : nullptr;
  if (state && string_assign && !overridden(item, use) && item_class &&
      usable_type(item_class, custom_label)) {
    // A usable registered type offers its action under its own label.
    state[kStateAvailable] = 1;
    state[kStateEnabled] = 1;
    string_assign(state + kStateReason, "");
    string_assign(state + kStateLabel, custom_label.c_str());
    return result;
  }
  if (!state || !string_assign || !overridden(item, use)) {
    return result;
  }
  const bool offered = state[kStateAvailable] != 0;
  state[kStateAvailable] = 1;
  state[kStateEnabled] = 1;
  string_assign(state + kStateReason, "");
  const char* label = at<const char*>(state, kStateLabel);
  if (!offered || !label || !*label) {
    string_assign(state + kStateLabel, "ui_invhint_use");
  }
  return result;
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install_use(const UseNatives& natives, Logger logger) {
  log_line = logger;
  string_copy = reinterpret_cast<StringCopy>(natives.string_copy);
  string_assign = reinterpret_cast<StringAssign>(natives.string_assign);
  item_owner = reinterpret_cast<ItemOwner>(natives.item_owner);
  use_natives = natives;
  if (!string_copy || !string_assign) {
    return false;
  }
  // Servings from pots and barrels go to the server too, when every native is there.
  if (natives.eat_item_bind && natives.handler_soul && natives.script_end_bool &&
      natives.soul_owner && natives.item_desc_init && natives.item_desc_free &&
      natives.inventory_add && natives.inventory_remove && natives.local_soul) {
    const bool hooked = hook(natives.eat_item_bind, reinterpret_cast<void*>(eat_item),
                             reinterpret_cast<void**>(&original_eat_item));
    if (log_line) {
      log_line("INVENTORY servings %s", hooked ? "decided by the server" : "unavailable");
    }
  }
  return hook(natives.consume, reinterpret_cast<void*>(consume),
              reinterpret_cast<void**>(&original_consume)) &&
         hook(natives.learn, reinterpret_cast<void*>(learn),
              reinterpret_cast<void**>(&original_learn)) &&
         hook(natives.primary, reinterpret_cast<void*>(primary),
              reinterpret_cast<void**>(&original_primary)) &&
         hook(natives.equip_toggle, reinterpret_cast<void*>(equip_toggle),
              reinterpret_cast<void**>(&original_equip_toggle)) &&
         hook(natives.primary_state, reinterpret_cast<void*>(primary_state),
              reinterpret_cast<void**>(&original_primary_state)) &&
         hook(natives.owner_use, reinterpret_cast<void*>(owner_use),
              reinterpret_cast<void**>(&original_owner_use));
}

bool handle_use_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "inventoryUseOverrides") {
    return false;
  }
  std::unordered_set<std::string> classes;
  std::unordered_set<std::uint64_t> uids;
  if (const sdk::Value* list = integration::script_field(message, "classes");
      list && list->tag() == sdk::Tag::Array) {
    for (const sdk::Value& entry : list->as_array()) {
      integration::Guid guid{};
      if (entry.tag() == sdk::Tag::Str && integration::parse_guid(entry.as_str(), guid)) {
        classes.insert(integration::guid_text(guid));
      }
    }
  }
  if (const sdk::Value* list = integration::script_field(message, "uids");
      list && list->tag() == sdk::Tag::Array) {
    for (const sdk::Value& entry : list->as_array()) {
      if (entry.tag() == sdk::Tag::Str) {
        try {
          uids.insert(std::stoull(entry.as_str()));
        } catch (...) {
        }
      }
    }
  }
  std::lock_guard<std::mutex> lock(overrides_mutex);
  override_classes = std::move(classes);
  override_uids = std::move(uids);
  if (log_line) {
    log_line("INVENTORY use overrides: %zu classes, %zu items", override_classes.size(),
             override_uids.size());
  }
  return true;
}

void pump_uses(const RuntimeSender& send) {
  std::vector<Use> taken;
  {
    std::lock_guard<std::mutex> lock(uses_mutex);
    taken.swap(uses);
  }
  for (const Use& use : taken) {
    // A use whose row went, or whose stock changed, since the press is dropped.
    std::string row;
    std::uint64_t revision = 0;
    if (!row_of(use.uid, row, revision) || row != use.row || revision != use.revision) {
      if (log_line) {
        log_line("INVENTORY use of row %s dropped: the stock changed", use.row.c_str());
      }
      continue;
    }
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("inventoryUse"))},
                                    {"item", sdk::Value(use.item)},
                                    {"uid", sdk::Value(std::to_string(use.uid))},
                                    {"id", sdk::Value(use.row)},
                                    {"revision", sdk::Value(double(use.revision))},
                                    {"amount", sdk::Value(1)},
                                    {"action", sdk::Value(std::string(use.action))}}));
    if (log_line) {
      log_line("INVENTORY %s use of row %s taken over", use.action, use.row.c_str());
    }
  }
}

void reset_uses() {
  {
    std::lock_guard<std::mutex> lock(consume_mutex);
    waiting.clear();
    consume_requests.clear();
  }
  std::lock_guard<std::mutex> lock(uses_mutex);
  uses.clear();
}

std::vector<CustomUse> take_custom_uses() {
  std::lock_guard<std::mutex> lock(custom_mutex);
  return std::exchange(custom_uses, {});
}

std::vector<ConsumeRequest> take_consume_requests() {
  std::lock_guard<std::mutex> lock(consume_mutex);
  return std::exchange(consume_requests, {});
}

std::optional<bool> apply_consume_verdict(const integration::InventoryConsume& verdict) {
  using Verdict = integration::InventoryConsume::Verdict;
  Waiting pending;
  {
    std::lock_guard<std::mutex> lock(consume_mutex);
    const auto found = waiting.find(verdict.request);
    if (found == waiting.end()) {
      return std::nullopt;
    }
    pending = found->second;
    waiting.erase(found);
  }
  if (verdict.verdict == Verdict::Refused) {
    if (log_line) {
      log_line("INVENTORY %s%s refused by the server", pending.pot ? "serving" : "use of row ",
               pending.row.c_str());
    }
    return std::nullopt;
  }
  if (pending.pot) {
    // A serving: the game's own eating, or nothing at all without its effects.
    const bool used = verdict.verdict == Verdict::Native ? eat_serving(pending.item) : true;
    if (log_line) {
      log_line("INVENTORY serving %s: %s", verdict.verdict == Verdict::Native ? "eaten" : "plain",
               used ? "used" : "failed");
    }
    return used;
  }
  // The item the player pressed, or another of its row if that one went meanwhile.
  const std::vector<void*> items = items_of(pending.row);
  void* item = items.empty() ? nullptr : items.front();
  for (void* candidate : items) {
    if (at<std::uint64_t>(candidate, kItemHandle) == pending.uid) {
      item = candidate;
      break;
    }
  }
  if (!item) {
    return false;
  }
  if (verdict.verdict == Verdict::Plain) {
    const bool used = take_unit(item);
    if (log_line) {
      log_line("INVENTORY use of row %s without its effects: %s", pending.row.c_str(),
               used ? "used" : "failed");
    }
    return used;
  }
  void* owner = item_owner ? item_owner(item) : nullptr;
  if (!owner) {
    return false;
  }
  unsigned result = 0;
  own_use = true;
  (*static_cast<OwnerUse**>(owner))[kOwnerUse](owner, &result, item, 1, -1, 0, -1);
  own_use = false;
  const bool used = (result & kUseFailed) == 0;
  if (log_line) {
    log_line("INVENTORY use of row %s as the game does it: %s (result 0x%x)", pending.row.c_str(),
             used ? "used" : "failed", result);
  }
  return used;
}

}  // namespace kcds::runtime::inventory
