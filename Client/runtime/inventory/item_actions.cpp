#include "item_actions.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../input/cuffs.hpp"
#include "../input/frozen.hpp"
#include "inventory_use.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "stock_projection.hpp"

namespace kcds::runtime::inventory::actions {
namespace {

namespace sdk = kcd2s::sdk;

using Primary = void*(__fastcall*)(void** panel, void*** item);
using PrimaryState = std::uint8_t*(__fastcall*)(void* panel, std::uint8_t* state, void* item);
using ItemOwner = void*(__fastcall*)(void* item);
using StringAssign = void*(__fastcall*)(void* text, const char* value);
using StringCall = void*(__fastcall*)(void* text);
using IsReadable = bool(__fastcall*)(void* unused, void* item);
using ReadItem = bool(__fastcall*)(void* human, void* item, void* error);
using CanRead = bool(__fastcall*)(void* human);
// The owner's use of an item (vtable + 1560): result flags, the item, an amount and the consume
// path's other arguments, as the food quickslot passes them.
using OwnerUse = void(__fastcall*)(void* owner, unsigned* result, void* item, int amount, int a,
                                   char b, int c);
constexpr std::size_t kOwnerUse = 1560 / 8;
// The use's flags: any of these, it failed; this one alone, the server was asked to decide.
constexpr unsigned kUseFailed = 0x1FE;
constexpr unsigned kUseAskedServer = 0x2;
// The primary action's state: offered, enabled, the text key of why not and the label, the two
// texts CryStrings (a pointer to their characters).
constexpr std::size_t kStateAvailable = 0;
constexpr std::size_t kStateEnabled = 1;
constexpr std::size_t kStateReason = 8;
constexpr std::size_t kStateLabel = 16;
constexpr std::size_t kStateSize = 64;
constexpr std::size_t kMaxQueued = 64;

struct Request {
  std::int64_t id;
  sdk::Value body;
};

struct Reply {
  std::int64_t id;
  sdk::Value value;
};

Natives natives;
std::mutex queue_mutex;
std::vector<Request> requests;
std::vector<Reply> replies;

template <class T>
T at(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

std::string text_at(const std::uint8_t* state, std::size_t offset) {
  const char* text = at<const char*>(state, offset);
  return text ? std::string(text) : std::string();
}

// The first native item of a row of the stock, or null.
void* item_of(const sdk::Value& request) {
  const sdk::Value* row = kcd2s::integration::script_field(request, "row");
  if (!row || row->tag() != sdk::Tag::Str || row->as_str().empty()) {
    return nullptr;
  }
  const std::vector<void*> items = items_of(row->as_str());
  return items.empty() ? nullptr : items.front();
}

bool readable(void* item) {
  return natives.is_readable && reinterpret_cast<IsReadable>(natives.is_readable)(nullptr, item);
}

sdk::Value state_value(bool available, bool enabled, const std::string& reason, const char* label) {
  return sdk::Value(sdk::Value::Map{
      {"available", sdk::Value(available)},
      {"enabled", sdk::Value(enabled)},
      {"reason", sdk::Value(reason)},
      {"label", sdk::Value(std::string(label))},
  });
}

// With the screen closed: food is offered as eaten, a document as read where reading is
// allowed; null for anything else.
sdk::Value closed_screen_state(void* item) {
  if (food_item(item)) {
    return state_value(true, true, "", "");
  }
  void* human = natives.local_human ? natives.local_human() : nullptr;
  if (readable(item) && human && natives.can_read) {
    const bool here = reinterpret_cast<CanRead>(natives.can_read)(human);
    return state_value(true, here, here ? "" : "ui_hud_cantReadHere", "ui_invhint_read");
  }
  return sdk::Value();
}

// The screen's state of the item's primary action: { available, enabled, reason, label }; with
// the screen closed, closed_screen_state.
sdk::Value state_of(void* item) {
  void* panel = natives.player_panel ? natives.player_panel() : nullptr;
  if (!panel || !natives.primary_state || !natives.string_assign) {
    return closed_screen_state(item);
  }
  alignas(16) std::uint8_t state[kStateSize] = {};
  reinterpret_cast<PrimaryState>(natives.primary_state)(panel, state, item);
  sdk::Value value(sdk::Value::Map{
      {"available", sdk::Value(state[kStateAvailable] != 0)},
      {"enabled", sdk::Value(state[kStateEnabled] != 0)},
      {"reason", sdk::Value(text_at(state, kStateReason))},
      {"label", sdk::Value(text_at(state, kStateLabel))},
  });
  // The texts are the game's own strings: given back.
  auto assign = reinterpret_cast<StringAssign>(natives.string_assign);
  assign(state + kStateReason, "");
  assign(state + kStateLabel, "");
  return value;
}

// Reads a document with the game's inventory read: null once its reading started, else why not.
const char* read(void* item) {
  void* human = natives.local_human ? natives.local_human() : nullptr;
  if (!human || !natives.read_item || !natives.string_init || !natives.string_free) {
    return "unavailable";
  }
  void* error = nullptr;
  reinterpret_cast<StringCall>(natives.string_init)(&error);
  const bool reading = reinterpret_cast<ReadItem>(natives.read_item)(human, item, &error);
  if (natives.log && !reading) {
    natives.log("INVENTORY scripted read refused (%s)",
                error ? static_cast<const char*>(error) : "");
  }
  reinterpret_cast<StringCall>(natives.string_free)(&error);
  return reading ? nullptr : "notAvailable";
}

// Uses the item: null once the game or the server has it, else why not.
const char* use(void* item) {
  if (input::cuffs::cuffed() || input::frozen::frozen()) {
    return "locked";
  }
  if (food_item(item)) {
    void* owner =
        natives.item_owner ? reinterpret_cast<ItemOwner>(natives.item_owner)(item) : nullptr;
    if (!owner) {
      return "unavailable";
    }
    unsigned result = 0;
    (*static_cast<OwnerUse**>(owner))[kOwnerUse](owner, &result, item, 1, -1, 0, -1);
    if (natives.log) {
      natives.log("INVENTORY scripted use of food: result 0x%x", result);
    }
    return result == kUseAskedServer || (result & kUseFailed) == 0 ? nullptr : "refused";
  }
  if (readable(item)) {
    return read(item);
  }
  void* panel = natives.player_panel ? natives.player_panel() : nullptr;
  if (!panel || !natives.primary) {
    return "unavailable";
  }
  alignas(16) std::uint8_t state[kStateSize] = {};
  if (natives.primary_state && natives.string_assign) {
    reinterpret_cast<PrimaryState>(natives.primary_state)(panel, state, item);
    const bool usable = state[kStateAvailable] != 0 && state[kStateEnabled] != 0;
    auto assign = reinterpret_cast<StringAssign>(natives.string_assign);
    assign(state + kStateReason, "");
    assign(state + kStateLabel, "");
    if (!usable) {
      return "notAvailable";
    }
  }
  void* chosen = item;
  void** chosen_slot = &chosen;
  void* screen_panel = panel;
  reinterpret_cast<Primary>(natives.primary)(&screen_panel, &chosen_slot);
  return nullptr;
}

sdk::Value answer(const sdk::Value& request, void* actor) {
  const std::string op = kcd2s::integration::script_operation(request);
  if (!actor) {
    return op == "itemUse" ? sdk::Value(std::string("notInWorld")) : sdk::Value();
  }
  void* item = item_of(request);
  if (op == "itemUseState") {
    return item ? state_of(item) : sdk::Value();
  }
  if (op != "itemUse") {
    return sdk::Value();
  }
  const char* reason = item ? use(item) : "noItem";
  if (natives.log) {
    natives.log("INVENTORY scripted use: %s", reason ? reason : "started");
  }
  return sdk::Value(std::string(reason ? reason : "started"));
}

}  // namespace

void install(const Natives& given) {
  natives = given;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "syncRequest") {
    return false;
  }
  const sdk::Value* id = kcd2s::integration::script_field(message, "id");
  const sdk::Value* request = kcd2s::integration::script_field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request ||
      kcd2s::integration::script_operation(*request).rfind("itemUse", 0) != 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (requests.size() < kMaxQueued) {
    requests.push_back(Request{id->as_int(), *request});
  }
  return true;
}

void frame(void* local_actor) {
  std::vector<Request> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(requests);
  }
  if (taken.empty()) {
    return;
  }
  std::vector<Reply> answered;
  for (const Request& request : taken) {
    answered.push_back(Reply{request.id, answer(request.body, local_actor)});
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  for (Reply& reply : answered) {
    replies.push_back(std::move(reply));
  }
}

void pump(const RuntimeSender& send) {
  std::vector<Reply> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(replies);
  }
  for (const Reply& reply : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(reply.id)},
                                    {"value", reply.value}}));
  }
}

}  // namespace kcds::runtime::inventory::actions
