#include "carry_items.hpp"

#include <windows.h>

#include <cstring>
#include <unordered_map>

#include "kcd2s/integration/carry.hpp"

namespace kcds::runtime::carry {
namespace {

namespace integration = kcd2s::integration;

// The carry action's parameters (S_CarryItemActionParams): the item's handle, the animation tag
// (a CryString: the text after a header of reference count, length and capacity), a flag.
struct ActionParams {
  std::uint64_t item = 0;
  const char* tag = nullptr;
  std::uint8_t flag = 0;
};

using CreateAction = void*(__fastcall*)(void* factory, void** action, const ActionParams* params);
using ActionStep = std::int64_t(__fastcall*)(void* action);
using FindItem = void*(__fastcall*)(void* registry, const std::uint64_t* handle);
using FindAction = void(__fastcall*)(void* actions, void** action, std::uint8_t type);
using StartAction = bool(__fastcall*)(void* actions, void** action);
using Reference = void(__fastcall*)(void* object);
using Restricted = bool(__fastcall*)(void* restrictions, std::uint8_t restriction);

constexpr std::size_t kActorActions = 640;
constexpr std::size_t kActionsFactory = 0x50;
constexpr std::size_t kActionsStart = 32 / 8;
constexpr std::size_t kActionsFind = 64 / 8;
constexpr std::uint8_t kCarryActionType = 29;
constexpr std::size_t kAddReference = 8 / 8;
constexpr std::size_t kReleaseReference = 16 / 8;
constexpr std::size_t kActionItem = 80;
constexpr std::size_t kActionPhase = 144;
constexpr std::uint8_t kPhaseTakingUp = 1;
constexpr std::uint8_t kPhaseCarried = 3;
constexpr std::uint8_t kPhasePuttingDown = 4;
constexpr std::uint8_t kPhasePutDown = 5;
// The actor's human (+ 0x990) keeps its restrictions at + 0x370; restriction 13 forbids a carry
// (as asks).
constexpr std::size_t kActorHuman = 0x990;
constexpr std::size_t kHumanRestrictions = 0x370;
constexpr std::size_t kRestrictionsIs = 8 / 8;
constexpr std::uint8_t kCarryRestriction = 13;
// An item's handle, its class, the class's GUID; the item manager's registry.
constexpr std::size_t kItemHandle = 0x30;
constexpr std::size_t kItemClass = 0x48;
constexpr std::size_t kClassGuid = 0x08;
constexpr std::size_t kRegistryItems = 0x168;
constexpr std::int32_t kNeverFreed = 0x3FFFFFFF;

// The empty animation tag: the item's own clips. Its reference count never reaches zero.
struct EmptyTag {
  std::int32_t references = kNeverFreed;
  std::int32_t length = 0;
  std::int32_t capacity = 0;
  char text[4] = {};
};
EmptyTag empty_tag;

// What a replica is shown carrying: the class, the item laid for it, and the tries to start its
// carry (a replica that cannot carry now is tried again a while later, a few times).
struct Shown {
  Guid item_class{};
  std::uint64_t item = 0;
  int tries = 0;
  std::uint64_t tried_at = 0;
};
constexpr int kMaxTries = 3;
constexpr std::uint64_t kRetryMs = 2000;

Natives natives;
Logger log_line = nullptr;
bool installed = false;
// The item laid or taken up for the local actor's carry, and its class; 0 for none.
std::uint64_t own_item = 0;
Guid own_class{};
// The field kept naming a carry for a moment after its item was reported let go.
std::string held_field;
std::uint64_t held_until = 0;
constexpr std::uint64_t kHoldMs = 300;
std::unordered_map<std::uint32_t, Shown> shown;

template <class Value>
Value at(const void* object, std::size_t offset) {
  Value value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

template <class Function>
Function virtual_function(void* object, std::size_t slot) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[slot]);
}

void release(void* object) {
  virtual_function<Reference>(object, kReleaseReference)(object);
}

void* item_of(std::uint64_t handle) {
  void* registry = natives.item_manager ? *natives.item_manager : nullptr;
  if (!registry || handle == 0) {
    return nullptr;
  }
  return reinterpret_cast<FindItem>(natives.find_item)(
      static_cast<std::uint8_t*>(registry) + kRegistryItems, &handle);
}

void* actions_of(void* actor) {
  return actor ? at<void*>(actor, kActorActions) : nullptr;
}

// The actor's running carry action with a reference the caller releases, or null.
void* carry_action(void* actor) {
  void* actions = actions_of(actor);
  if (!actions) {
    return nullptr;
  }
  void* action = nullptr;
  virtual_function<FindAction>(actions, kActionsFind)(actions, &action, kCarryActionType);
  return action;
}

bool restricted(void* actor) {
  void* human = at<void*>(actor, kActorHuman);
  if (!human) {
    return true;
  }
  void* restrictions = static_cast<std::uint8_t*>(human) + kHumanRestrictions;
  return virtual_function<Restricted>(restrictions, kRestrictionsIs)(restrictions,
                                                                     kCarryRestriction);
}

// The handle of the item the actor carries, 0 for none.
std::uint64_t carried_handle(void* actor) {
  void* action = carry_action(actor);
  if (!action) {
    return 0;
  }
  const auto phase = at<std::uint8_t>(action, kActionPhase);
  const auto handle = at<std::uint64_t>(action, kActionItem);
  release(action);
  return phase < kPhasePutDown ? handle : 0;
}

// Takes away an item laid for a carry that is over, if it is still about.
void take_away_laid(std::uint64_t handle) {
  if (void* item = item_of(handle)) {
    natives.take_away(item);
  }
}

// Starts the carry of a laid item: the actor takes it up with the game's clip.
bool start_carry(void* actor, std::uint64_t handle) {
  void* actions = actions_of(actor);
  const ActionParams params{handle, empty_tag.text, 0};
  void* action = nullptr;
  reinterpret_cast<CreateAction>(natives.create_action)(at<void*>(actions, kActionsFactory),
                                                        &action, &params);
  if (!action) {
    return false;
  }
  void* started = action;
  virtual_function<Reference>(action, kAddReference)(action);
  const bool running = virtual_function<StartAction>(actions, kActionsStart)(actions, &started);
  release(action);
  return running;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  installed = natives.create_action && natives.put_down && natives.let_go && natives.find_item &&
              natives.item_manager && natives.lay && natives.take_away && natives.lies_in_world &&
              natives.item_position && natives.report_put_down;
  return installed;
}

bool carried(void* actor, Guid& item_class) {
  if (!installed) {
    return false;
  }
  void* item = item_of(carried_handle(actor));
  void* class_object = item ? at<void*>(item, kItemClass) : nullptr;
  if (!class_object) {
    return false;
  }
  std::memcpy(item_class.data(), static_cast<const std::uint8_t*>(class_object) + kClassGuid,
              item_class.size());
  return true;
}

bool carry_new(void* actor, const Guid& item_class, const float* position) {
  const char* refusal = nullptr;
  if (!installed || !actions_of(actor)) {
    refusal = "no actions";
  } else if (void* action = carry_action(actor)) {
    release(action);
    refusal = "carries already";
  } else if (restricted(actor)) {
    refusal = "restricted";
  }
  void* item = refusal ? nullptr : natives.lay(item_class, position);
  if (!refusal && !item) {
    refusal = "item not laid";
  }
  const auto handle = item ? at<std::uint64_t>(item, kItemHandle) : 0;
  if (item && !start_carry(actor, handle)) {
    natives.take_away(item);
    refusal = "action not started";
  }
  if (refusal) {
    if (log_line) {
      log_line("CARRY new carry refused: %s", refusal);
    }
    return false;
  }
  own_item = handle;
  own_class = item_class;
  return true;
}

bool carry_laid(void* actor, void* item) {
  void* class_object = item ? at<void*>(item, kItemClass) : nullptr;
  if (!installed || !actions_of(actor) || !class_object) {
    return false;
  }
  if (void* action = carry_action(actor)) {
    release(action);
    return false;
  }
  if (restricted(actor)) {
    return false;
  }
  const auto handle = at<std::uint64_t>(item, kItemHandle);
  if (!start_carry(actor, handle)) {
    if (log_line) {
      log_line("CARRY stack carry refused: action not started");
    }
    return false;
  }
  own_item = handle;
  std::memcpy(own_class.data(), static_cast<const std::uint8_t*>(class_object) + kClassGuid,
              own_class.size());
  return true;
}

bool put_down(void* actor, bool at_once) {
  void* action = installed ? carry_action(actor) : nullptr;
  if (!action) {
    return false;
  }
  const auto phase = at<std::uint8_t>(action, kActionPhase);
  bool done = false;
  if (at_once && phase >= kPhaseTakingUp && phase <= kPhasePuttingDown) {
    reinterpret_cast<ActionStep>(natives.let_go)(action);
    done = true;
  } else if (!at_once && phase == kPhaseCarried) {
    reinterpret_cast<ActionStep>(natives.put_down)(action);
    done = true;
  }
  release(action);
  return done;
}

std::string capture(void* actor) {
  const std::uint64_t handle = installed ? carried_handle(actor) : 0;
  if (own_item != 0 && handle != own_item) {
    // The carry is over: an item let go in the world is reported where it lies, then goes.
    void* item = item_of(own_item);
    float position[3] = {};
    if (item && natives.lies_in_world(item) && natives.item_position(item, position)) {
      natives.report_put_down(own_class, position);
      held_field = integration::carry_field(&own_class);
      held_until = GetTickCount64() + kHoldMs;
      if (log_line) {
        log_line("CARRY let go at %.2f %.2f %.2f", position[0], position[1], position[2]);
      }
    }
    take_away_laid(own_item);
    own_item = 0;
  }
  Guid item_class{};
  if (!carried(actor, item_class)) {
    return GetTickCount64() < held_until ? held_field : std::string();
  }
  held_until = 0;
  return integration::carry_field(&item_class);
}

void keep_replica(std::uint32_t player, void* actor, const float* position,
                  const std::string& field) {
  if (!installed || !actor) {
    return;
  }
  Guid wanted{};
  const bool carries = integration::parse_carry_field(field, wanted);
  auto found = shown.find(player);
  if (!carries) {
    if (found != shown.end()) {
      put_down(actor, true);
      take_away_laid(found->second.item);
      shown.erase(found);
      if (log_line) {
        log_line("CARRY replica %u put down", player);
      }
    }
    return;
  }
  if (found == shown.end() || found->second.item_class != wanted) {
    if (found != shown.end()) {
      put_down(actor, true);
      take_away_laid(found->second.item);
    }
    found = shown.insert_or_assign(player, Shown{wanted}).first;
  }
  Shown& replica = found->second;
  if (replica.item != 0 && carried_handle(actor) == replica.item) {
    return;
  }
  const std::uint64_t now = GetTickCount64();
  if (replica.tries >= kMaxTries || (replica.tries > 0 && now - replica.tried_at < kRetryMs)) {
    return;
  }
  replica.tries += 1;
  replica.tried_at = now;
  take_away_laid(replica.item);
  replica.item = 0;
  const char* refusal = nullptr;
  if (carried_handle(actor) != 0) {
    refusal = "carries already";
  } else if (restricted(actor)) {
    refusal = "restricted";
  }
  void* item = refusal ? nullptr : natives.lay(wanted, position);
  if (!refusal && !item) {
    refusal = "item not laid";
  }
  const auto handle = item ? at<std::uint64_t>(item, kItemHandle) : 0;
  if (item && !start_carry(actor, handle)) {
    natives.take_away(item);
    refusal = "action not started";
  }
  if (refusal) {
    if (log_line) {
      log_line("CARRY replica %u carry refused (try %d): %s", player, replica.tries, refusal);
    }
    return;
  }
  replica.item = handle;
  if (log_line) {
    log_line("CARRY replica %u carries %s (try %d)", player, field.c_str(), replica.tries);
  }
}

void forget(std::uint32_t player) {
  auto found = shown.find(player);
  if (found == shown.end()) {
    return;
  }
  take_away_laid(found->second.item);
  shown.erase(found);
}

void reset() {
  shown.clear();
  own_item = 0;
}

}  // namespace kcds::runtime::carry
