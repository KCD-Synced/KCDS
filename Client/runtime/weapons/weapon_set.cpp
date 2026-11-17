#include "weapon_set.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <typeinfo>
#include <vector>

#include "../core/game_layouts.hpp"
#include "../input/cuffs.hpp"
#include "../input/frozen.hpp"
#include "drawn_weapons.hpp"
#include "kcd2s/integration/script_channel.hpp"

#include "MinHook.h"

namespace kcds::runtime::weapons::set {
namespace {

namespace sdk = kcd2s::sdk;

constexpr std::size_t kActorSoul = kcds::layouts::Actor::kSoul;
constexpr std::size_t kActorPlayerInput = 2952;
constexpr std::size_t kPlayerInputActor = 16;
// The soul's items, their quick access and its weapon manager (virtual getters).
constexpr std::size_t kSoulItems = 480 / 8;
constexpr std::size_t kItemsQuickAccess = 392 / 8;
constexpr std::size_t kQuickAccessWeapons = 128 / 8;
// The weapon manager.
constexpr std::size_t kSelect = 40 / 8;
constexpr std::size_t kSlotUsable = 272 / 8;
constexpr std::size_t kRunSelected = 72 / 8;
constexpr std::size_t kSelectedSlot = 96 / 8;
constexpr std::size_t kSlotAllowed = 120 / 8;
constexpr std::size_t kSlots = 48;
constexpr std::size_t kSlotSize = 48;
constexpr std::size_t kSlotSecondary = 8;
constexpr unsigned kSlotCount = 4;
// The game's finish reason of an action that ran to its end.
constexpr std::uint8_t kFinishedNormally = 0;
// How long a torch toggle may take to show in the left hand, and a weapon action to end, seconds.
constexpr double kTorchSeconds = 3.0;
constexpr double kActionSeconds = 10.0;
constexpr std::size_t kMaxQueued = 64;
// The draw key: its action, how long a press may last to be a tap (the key's holdTriggerDelay
// in the game's defaultProfile.xml) and how long after the release the game may act first.
constexpr char kDrawKeyAction[] = "toggle_holster_qam_weapon";
constexpr double kTapSeconds = 0.25;
constexpr double kKeyWaitSeconds = 0.35;

using Getter = void*(__fastcall*)(void* object);
using SelectedSlot = std::uint32_t(__fastcall*)(void* manager);
using SlotAllowed = bool(__fastcall*)(void* manager, std::uint32_t slot);
using Select = void*(__fastcall*)(void* manager, unsigned slot, char on);
using TorchToggle = void(__fastcall*)(void* input);
using IsTorch = bool(__fastcall*)(void* item);

template <class T>
T at(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

template <class Function>
Function slot(void* object, std::size_t index) {
  return (*static_cast<Function**>(object))[index];
}

double seconds_now() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// The finish callback handed to the manager: an MSVC std::function<void(FinishReason)> whose
// callable (a ticket) lives in the function's own storage. The game moves and destroys it as
// its own; the call reports the reason for the ticket.
struct FinishCallable {
  const void* const* table;
  std::uint32_t ticket;
};

struct GameFunction {
  alignas(8) std::uint8_t storage[56];
  void* impl;
};
static_assert(sizeof(GameFunction) == 64, "an MSVC std::function is 64 bytes");

void finished(std::uint32_t ticket, std::uint8_t reason);

void* __fastcall callable_copy(FinishCallable* self, void* where) {
  auto* copy = static_cast<FinishCallable*>(where);
  copy->table = self->table;
  copy->ticket = self->ticket;
  return copy;
}

void __fastcall callable_call(FinishCallable* self, std::uint8_t* reason) {
  finished(self->ticket, reason ? *reason : kFinishedNormally);
}

const std::type_info* __fastcall callable_type(FinishCallable*) {
  return &typeid(FinishCallable);
}

void __fastcall callable_delete(FinishCallable*, bool) {
}

const void* __fastcall callable_get(FinishCallable* self) {
  return &self->ticket;
}

// _Copy, _Move, _Do_call, _Target_type, _Delete_this, _Get.
const void* const kCallableTable[] = {
    reinterpret_cast<const void*>(&callable_copy),   reinterpret_cast<const void*>(&callable_copy),
    reinterpret_cast<const void*>(&callable_call),   reinterpret_cast<const void*>(&callable_type),
    reinterpret_cast<const void*>(&callable_delete), reinterpret_cast<const void*>(&callable_get),
};

using RunSelected = bool(__fastcall*)(void* manager, GameFunction* done);
using RunSlot = void*(__fastcall*)(void* slot, std::uint8_t* result, void* actor, char animated,
                                   int selected, void* done);
RunSlot original_run_slot = nullptr;
// Set while this client's own call runs, so the hook keeps the game's result code of that call.
bool capturing = false;
int captured_code = -1;

void* __fastcall hooked_run_slot(void* slot_entry, std::uint8_t* result, void* actor, char animated,
                                 int selected, void* done) {
  void* answer = original_run_slot(slot_entry, result, actor, animated, selected, done);
  if (capturing && result) {
    captured_code = result[0];
  }
  return answer;
}

struct Request {
  std::int64_t id;
  sdk::Value body;
};

struct Reply {
  std::int64_t id;
  sdk::Value value;
};

struct Event {
  std::string action;
  bool ok;
  std::string reason;
};

Natives natives;
std::mutex queue_mutex;
std::vector<Request> requests;
std::vector<Reply> replies;
std::vector<Event> events;

// The running action (engine thread): its name ('draw', 'holster', 'torchDraw',
// 'torchHolster'), its ticket and when it started; none when action is empty.
std::string action;
std::uint32_t ticket = 0;
// The draw key (engine thread): when it went down and whether the set was drawn then, and when a
// tap of it is carried out unless the game acted (negative: none).
double key_down_at = -1;
bool key_drawn_before = false;
double key_check_at = -1;
double started_at = 0.0;
bool torch_wanted = false;

void raise(const std::string& name, bool ok, const char* reason) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (events.size() < kMaxQueued) {
    events.push_back(Event{name, ok, reason});
  }
}

void finished(std::uint32_t done, std::uint8_t reason) {
  if (action.empty() || done != ticket) {
    return;
  }
  const bool ok = reason == kFinishedNormally;
  raise(action, ok, ok ? "" : "interrupted");
  if (natives.log && !ok) {
    natives.log("WEAPONS %s ended with reason %u", action.c_str(), unsigned(reason));
  }
  action.clear();
}

void* weapon_manager(void* actor) {
  void* soul = actor ? at<void*>(actor, kActorSoul) : nullptr;
  void* items = soul ? slot<Getter>(soul, kSoulItems)(soul) : nullptr;
  void* quick_access = items ? slot<Getter>(items, kItemsQuickAccess)(items) : nullptr;
  return quick_access ? slot<Getter>(quick_access, kQuickAccessWeapons)(quick_access) : nullptr;
}

void* slot_item(void* manager, std::uint32_t index, bool secondary) {
  return at<void*>(manager, kSlots + kSlotSize * index + (secondary ? kSlotSecondary : 0));
}

bool in_hand(void* actor, void* item) {
  return item && (item_in_hand(actor, false) == item || item_in_hand(actor, true) == item);
}

struct State {
  void* manager = nullptr;
  std::uint32_t slot = 0;
  bool has_weapon = false;
  bool drawn = false;
  bool allowed = false;
  bool torch = false;
};

State state_of(void* actor) {
  State state;
  state.manager = weapon_manager(actor);
  if (state.manager) {
    const std::uint32_t selected = slot<SelectedSlot>(state.manager, kSelectedSlot)(state.manager);
    state.slot = selected < kSlotCount ? selected : 0;
    void* primary = slot_item(state.manager, state.slot, false);
    void* secondary = slot_item(state.manager, state.slot, true);
    state.has_weapon = primary || secondary;
    state.drawn = in_hand(actor, primary) || in_hand(actor, secondary);
    state.allowed = slot<SlotAllowed>(state.manager, kSlotAllowed)(state.manager, state.slot);
  }
  void* left = item_in_hand(actor, true);
  state.torch = left && natives.is_torch && reinterpret_cast<IsTorch>(natives.is_torch)(left);
  return state;
}

sdk::Value state_value(void* actor) {
  const State state = state_of(actor);
  return sdk::Value(sdk::Value::Map{
      {"available", sdk::Value(state.manager != nullptr)},
      {"slot", sdk::Value(std::int64_t(state.slot))},
      {"hasWeapon", sdk::Value(state.has_weapon)},
      {"drawn", sdk::Value(state.drawn)},
      {"allowed", sdk::Value(state.allowed)},
      {"torch", sdk::Value(state.torch)},
      {"busy", sdk::Value(!action.empty())},
  });
}

// Why the local player cannot act now, or null.
const char* locked() {
  if (input::cuffs::cuffed() || input::frozen::frozen()) {
    return "locked";
  }
  if (!action.empty()) {
    return "busy";
  }
  return nullptr;
}

// Draws ('draw'), holsters ('holster') or toggles ('toggle') the selected set: null once the
// game's action started, else why not.
const char* run_weapon(void* actor, const std::string& wanted) {
  if (const char* reason = locked()) {
    return reason;
  }
  const State state = state_of(actor);
  if (!state.manager) {
    return "unavailable";
  }
  if (!state.has_weapon) {
    return "noWeapon";
  }
  if (wanted == "draw" && state.drawn) {
    return "alreadyDrawn";
  }
  if (wanted == "holster" && !state.drawn) {
    return "notDrawn";
  }
  if (!state.allowed) {
    return "notAllowed";
  }
  GameFunction done{};
  auto* callable = reinterpret_cast<FinishCallable*>(done.storage);
  callable->table = kCallableTable;
  callable->ticket = ++ticket;
  done.impl = callable;
  action = state.drawn ? "holster" : "draw";
  started_at = seconds_now();
  capturing = true;
  captured_code = -1;
  const bool started = slot<RunSelected>(state.manager, kRunSelected)(state.manager, &done);
  capturing = false;
  if (!started) {
    if (natives.log) {
      natives.log("WEAPONS %s refused by the game (result %d)", action.c_str(), captured_code);
    }
    // The animated holster finds no animation for a player's body here (its item handling's
    // prepare answers none, result 2), for the game's own key alike: the weapon goes back where
    // it hangs as human:HolsterWeapon puts it, at once, still equipped and in its set.
    if (state.drawn && weapons::holster(actor)) {
      raise(action, true, "");
      action.clear();
      return nullptr;
    }
    action.clear();
    return "refused";
  }
  return nullptr;
}

// Selects weapon set `wanted` (0 to 3) as the game's quick access does, without drawing it: null
// once selected, else why not.
const char* select_set(void* actor, std::int64_t wanted) {
  if (const char* reason = locked()) {
    return reason;
  }
  void* manager = weapon_manager(actor);
  if (!manager) {
    return "unavailable";
  }
  if (wanted < 0 || wanted >= std::int64_t(kSlotCount)) {
    return "invalid";
  }
  const auto index = unsigned(wanted);
  // The outfit has no such set (belts and sheaths add them).
  if (!slot<SlotAllowed>(manager, kSlotUsable)(manager, index)) {
    return "notAllowed";
  }
  if (slot<SelectedSlot>(manager, kSelectedSlot)(manager) != index) {
    slot<Select>(manager, kSelect)(manager, index, 1);
  }
  return nullptr;
}

const char* run_torch(void* actor, const std::string& wanted) {
  if (const char* reason = locked()) {
    return reason;
  }
  void* input = at<void*>(actor, kActorPlayerInput);
  if (!natives.torch_toggle || !natives.is_torch || !input ||
      at<void*>(input, kPlayerInputActor) != actor) {
    return "unavailable";
  }
  const bool drawn = state_of(actor).torch;
  if (wanted == "draw" && drawn) {
    return "alreadyDrawn";
  }
  if (wanted == "holster" && !drawn) {
    return "notDrawn";
  }
  reinterpret_cast<TorchToggle>(natives.torch_toggle)(input);
  action = drawn ? "torchHolster" : "torchDraw";
  torch_wanted = !drawn;
  ++ticket;
  started_at = seconds_now();
  return nullptr;
}

sdk::Value answer(const sdk::Value& request, void* actor) {
  const std::string op = kcd2s::integration::script_operation(request);
  if (!actor) {
    return op == "weaponsState" ? sdk::Value() : sdk::Value(std::string("notInWorld"));
  }
  if (op == "weaponsState") {
    return state_value(actor);
  }
  if (op == "weaponsSelect") {
    const sdk::Value* index = kcd2s::integration::script_field(request, "slot");
    std::int64_t wanted = -1;
    if (index && index->tag() == sdk::Tag::Int) {
      wanted = index->as_int();
    } else if (index && index->tag() == sdk::Tag::Double) {
      wanted = std::int64_t(index->as_double());
    }
    const char* reason = select_set(actor, wanted);
    if (natives.log) {
      natives.log("WEAPONS select %lld: %s", static_cast<long long>(wanted),
                  reason ? reason : "selected");
    }
    return sdk::Value(std::string(reason ? reason : "started"));
  }
  const sdk::Value* how = kcd2s::integration::script_field(request, "how");
  const std::string wanted = how && how->tag() == sdk::Tag::Str ? how->as_str() : "";
  if (wanted != "draw" && wanted != "holster" && wanted != "toggle") {
    return sdk::Value(std::string("invalid"));
  }
  const char* reason = nullptr;
  if (op == "weaponsSet") {
    reason = run_weapon(actor, wanted);
  } else if (op == "weaponsTorch") {
    reason = run_torch(actor, wanted);
  } else {
    return sdk::Value();
  }
  if (natives.log) {
    natives.log("WEAPONS %s %s: %s", op.c_str(), wanted.c_str(), reason ? reason : "started");
  }
  return sdk::Value(std::string(reason ? reason : "started"));
}

// A torch toggle shows in the left hand; a weapon action that never reports its end (the game
// dropped it) is let go.
void watch(void* actor) {
  if (action.empty()) {
    return;
  }
  const bool torch = action == "torchDraw" || action == "torchHolster";
  const double waited = seconds_now() - started_at;
  if (torch) {
    if (actor && state_of(actor).torch == torch_wanted) {
      raise(action, true, "");
      action.clear();
    } else if (waited > kTorchSeconds) {
      raise(action, false, "refused");
      action.clear();
    }
    return;
  }
  if (waited > kActionSeconds) {
    raise(action, false, "interrupted");
    action.clear();
  }
}

// A tap of the draw key the game did not act on by now: the selected set drawn or holstered as
// Weapons.toggle does.
void carry_out_key_tap(void* actor) {
  if (key_check_at < 0 || seconds_now() < key_check_at) {
    return;
  }
  key_check_at = -1;
  if (!actor || !action.empty()) {
    return;
  }
  const State state = state_of(actor);
  if (!state.has_weapon || state.drawn != key_drawn_before) {
    return;
  }
  const char* reason = run_weapon(actor, "toggle");
  if (natives.log) {
    natives.log("WEAPONS draw key tap: %s", reason ? reason : "started");
  }
}

}  // namespace

bool drawn(void* actor) {
  return actor && state_of(actor).drawn;
}

void on_action(void* local_actor, const char* name, unsigned mode) {
  if (!name || std::strcmp(name, kDrawKeyAction) != 0) {
    return;
  }
  const double now = seconds_now();
  if (mode == 1) {
    key_down_at = now;
    key_drawn_before = local_actor && state_of(local_actor).drawn;
    return;
  }
  if (mode == 2 && key_down_at >= 0) {
    if (now - key_down_at <= kTapSeconds) {
      key_check_at = now + kKeyWaitSeconds;
    }
    key_down_at = -1;
  }
}

void install(const Natives& given) {
  natives = given;
  const bool hooked = natives.run_slot &&
                      MH_CreateHook(natives.run_slot, reinterpret_cast<void*>(&hooked_run_slot),
                                    reinterpret_cast<void**>(&original_run_slot)) == MH_OK &&
                      MH_EnableHook(natives.run_slot) == MH_OK;
  if (natives.log && !hooked) {
    natives.log("WEAPONS the game's reasons for a refused set action are unavailable");
  }
  if (natives.log) {
    natives.log("WEAPONS set actions installed; torch %s",
                natives.torch_toggle && natives.is_torch ? "available" : "unavailable");
  }
}

bool handle_runtime_message(const sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "syncRequest") {
    return false;
  }
  const sdk::Value* id = kcd2s::integration::script_field(message, "id");
  const sdk::Value* request = kcd2s::integration::script_field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request ||
      kcd2s::integration::script_operation(*request).rfind("weapons", 0) != 0) {
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
  std::vector<Reply> answered;
  for (const Request& request : taken) {
    answered.push_back(Reply{request.id, answer(request.body, local_actor)});
  }
  if (!local_actor && !action.empty()) {
    raise(action, false, "interrupted");
    action.clear();
  }
  watch(local_actor);
  carry_out_key_tap(local_actor);
  std::lock_guard<std::mutex> lock(queue_mutex);
  for (Reply& reply : answered) {
    replies.push_back(std::move(reply));
  }
}

void pump(const RuntimeSender& send) {
  std::vector<Reply> taken_replies;
  std::vector<Event> taken_events;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken_replies.swap(replies);
    taken_events.swap(events);
  }
  for (const Reply& reply : taken_replies) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(reply.id)},
                                    {"value", reply.value}}));
  }
  for (const Event& event : taken_events) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("weaponsEvent"))},
                                    {"action", sdk::Value(event.action)},
                                    {"ok", sdk::Value(event.ok)},
                                    {"reason", sdk::Value(event.reason)}}));
  }
}

}  // namespace kcds::runtime::weapons::set
