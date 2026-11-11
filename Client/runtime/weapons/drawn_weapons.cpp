#include "drawn_weapons.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../core/game_layouts.hpp"
#include "kcd2s/integration/hands.hpp"

namespace kcds::runtime::weapons {
namespace {

using kcd2s::integration::HandItemBindings;
using kcd2s::integration::Hands;
using kcd2s::integration::ItemClassId;
using kcd2s::integration::ItemLifetimeTokens;
using kcd2s::integration::LocalHandItem;
using kcd2s::integration::LocalHandItems;

using DrawItem = std::uint64_t(__fastcall*)(void* controller, std::uint64_t item, int mode);
using HolsterItem = char(__fastcall*)(void* controller, std::uint64_t item);
using FindItem = void*(__fastcall*)(void* items, const std::uint64_t* item);

// Actor: its weapon controller (vtable slot 191) and item holders.
constexpr std::size_t kWeaponControllerSlot = 191;
constexpr std::size_t kItemHolders = 2448;
// Item holders: the item WUID in each hand.
constexpr std::size_t kRightHand = 608;
constexpr std::size_t kLeftHand = 616;
// Weapon controller: the sets, two item WUIDs each.
constexpr std::size_t kWeaponSets = 72;
constexpr std::size_t kWeaponSetItems = 6;
// The item manager's items; an item's WUID, its class and the class id.
constexpr std::size_t kManagerItems = 360;
constexpr std::size_t kItemWuid = kcds::layouts::ItemInstance::kHandle;
constexpr std::size_t kItemClass = kcds::layouts::ItemInstance::kClass;
constexpr std::size_t kClassId = 8;
// DrawWeapon's way of putting an item in the hand.
constexpr int kDrawMode = 2;

// What a replica was last made to hold: its owner's field and the replica's weapon sets then, what
// its hands held since (a draw takes the hand's animation), and the items made for it outside its
// sets (one per class).
struct Replica {
  void* actor = nullptr;
  std::string field;
  std::string shown_field;
  bool presentation_initialized = false;
  Hands wanted;
  bool valid_field = false;
  HandItemBindings bindings;
  std::array<std::uint64_t, kWeaponSetItems> sets{};
  std::array<std::uint64_t, 2> held{};
  std::vector<std::pair<ItemClassId, std::uint64_t>> extra;
};

Natives natives;
Logger log_line = nullptr;
std::unordered_map<std::uint32_t, Replica> replicas;
std::unordered_map<void*, std::uint32_t> replica_players;
void* owner_actor = nullptr;
ItemLifetimeTokens owner_tokens;
HandItemBindings owner_bindings;

template <class T>
T read(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

void* controller_of(void* actor) {
  return slot<void*(__fastcall*)(void*)>(actor, kWeaponControllerSlot)(actor);
}

void* item_of(std::uint64_t wuid) {
  void* manager = natives.item_manager ? *natives.item_manager : nullptr;
  if (!wuid || !manager) {
    return nullptr;
  }
  void* items = static_cast<std::uint8_t*>(manager) + kManagerItems;
  return reinterpret_cast<FindItem>(natives.find_item)(items, &wuid);
}

std::optional<ItemClassId> class_of(std::uint64_t wuid) {
  void* item = item_of(wuid);
  void* item_class = item ? read<void*>(item, kItemClass) : nullptr;
  if (!item_class) {
    return std::nullopt;
  }
  return read<ItemClassId>(item_class, kClassId);
}

std::optional<LocalHandItem> local_item(std::uint64_t uid) {
  void* item = item_of(uid);
  void* item_class = item ? read<void*>(item, kItemClass) : nullptr;
  if (!item_class || read<std::uint64_t>(item, kItemWuid) != uid) {
    return std::nullopt;
  }
  return LocalHandItem{uid, reinterpret_cast<std::uintptr_t>(item),
                       read<ItemClassId>(item_class, kClassId)};
}

// The item WUIDs in an actor's hands (right, left).
std::array<std::uint64_t, 2> hand_items(void* actor) {
  void* holders = read<void*>(actor, kItemHolders);
  if (!holders) {
    return {};
  }
  return {read<std::uint64_t>(holders, kRightHand), read<std::uint64_t>(holders, kLeftHand)};
}

LocalHandItems observed_hands(void* actor) {
  LocalHandItems observed{};
  if (!actor || !natives.find_item) {
    return observed;
  }
  const auto held = hand_items(actor);
  for (std::size_t hand = 0; hand < held.size(); ++hand) {
    observed[hand] = local_item(held[hand]);
  }
  return observed;
}

Hands refresh_owner(void* actor) {
  owner_tokens.retain_valid(local_item);
  const auto current = observed_hands(actor);
  Hands hands;
  if (current[0]) {
    hands.right = current[0]->item_class;
    hands.right_token = owner_tokens.token_for(*current[0], current).value_or(0);
  }
  if (current[1]) {
    hands.left = current[1]->item_class;
    hands.left_token = owner_tokens.token_for(*current[1], current).value_or(0);
  }
  owner_bindings.bind(hands, current);
  return hands;
}

std::array<std::uint64_t, kWeaponSetItems> weapon_sets(void* controller) {
  std::array<std::uint64_t, kWeaponSetItems> sets{};
  for (std::size_t i = 0; i < sets.size(); ++i) {
    sets[i] = read<std::uint64_t>(controller, kWeaponSets + 8 * i);
  }
  return sets;
}

// Whether a hand that held an item no longer holds it (the replica's game took it out of the hand,
// live: a replica hit by an arrow let its drawn bow go).
bool dropped(const std::array<std::uint64_t, 2>& before, const std::array<std::uint64_t, 2>& now) {
  for (std::size_t hand = 0; hand < before.size(); ++hand) {
    if (before[hand] && now[hand] != before[hand]) {
      return true;
    }
  }
  return false;
}

// The item of a class in the replica's weapon sets, 0 without one.
std::uint64_t set_item_of_class(const std::array<std::uint64_t, kWeaponSetItems>& sets,
                                const ItemClassId& id) {
  for (std::uint64_t wuid : sets) {
    if (wuid && class_of(wuid) == id) {
      return wuid;
    }
  }
  return 0;
}

// The item of a class the replica holds outside its weapon sets (one its owner drew from a
// quickslot): the one made for it before while it exists, else one made in its inventory now.
std::uint64_t extra_item_of_class(std::uint32_t player, Replica& replica, void* entity,
                                  const ItemClassId& id) {
  for (const auto& [item_class, wuid] : replica.extra) {
    if (item_class == id && item_of(wuid)) {
      return wuid;
    }
  }
  if (!natives.create_item || !entity) {
    return 0;
  }
  void* item = natives.create_item(entity, id);
  if (!item) {
    return 0;
  }
  const auto wuid = read<std::uint64_t>(item, kItemWuid);
  replica.extra.emplace_back(id, wuid);
  if (log_line) {
    log_line("WEAPONS player=%u made %s outside the weapon sets", player,
             kcd2s::integration::item_class_text(id).substr(0, 8).c_str());
  }
  return wuid;
}

// Makes the replica hold what hands names: holsters what it holds that is not named, then draws
// what it does not hold.
void hold(std::uint32_t player, Replica& replica, void* controller, void* actor, void* entity,
          const Hands& hands, const std::array<std::uint64_t, kWeaponSetItems>& sets) {
  const auto held = hand_items(actor);
  const std::optional<ItemClassId> wanted[2] = {hands.right, hands.left};
  bool in_hand[2] = {false, false};
  for (std::size_t hand = 0; hand < 2; ++hand) {
    if (!held[hand]) {
      continue;
    }
    const auto held_class = class_of(held[hand]);
    if (held_class && held_class == wanted[hand]) {
      in_hand[hand] = true;
      continue;
    }
    reinterpret_cast<HolsterItem>(natives.holster_item)(controller, held[hand]);
  }
  for (std::size_t hand = 0; hand < 2; ++hand) {
    if (!wanted[hand] || in_hand[hand]) {
      continue;
    }
    std::uint64_t item = set_item_of_class(sets, *wanted[hand]);
    if (!item) {
      item = extra_item_of_class(player, replica, entity, *wanted[hand]);
    }
    if (item) {
      reinterpret_cast<DrawItem>(natives.draw_item)(controller, item, kDrawMode);
    }
    if (log_line) {
      log_line("WEAPONS player=%u %s %s hand %s", player, item ? "draws" : "has no",
               hand == 0 ? "right" : "left",
               kcd2s::integration::item_class_text(*wanted[hand]).substr(0, 8).c_str());
    }
  }
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  return natives.draw_item && natives.holster_item && natives.find_item && natives.item_manager;
}

void* item_in_hand(void* actor, bool left) {
  void* holders = actor && natives.find_item ? read<void*>(actor, kItemHolders) : nullptr;
  return holders ? item_of(read<std::uint64_t>(holders, left ? kLeftHand : kRightHand)) : nullptr;
}

bool class_in_hand(void* actor, bool left, std::array<std::uint8_t, 16>& item_class) {
  void* item = item_in_hand(actor, left);
  void* type = item ? read<void*>(item, kItemClass) : nullptr;
  if (!type) {
    return false;
  }
  item_class = read<std::array<std::uint8_t, 16>>(type, kClassId);
  return true;
}

std::uint64_t striking_item(void* actor) {
  if (!actor) {
    return 0;
  }
  const auto held = hand_items(actor);
  return held[0] ? held[0] : held[1];
}

bool draw_set_item(void* actor, bool (*pick)(void* item)) {
  if (!actor || !pick || !natives.draw_item) {
    return false;
  }
  void* controller = controller_of(actor);
  if (!controller) {
    return false;
  }
  for (const std::uint64_t wuid : weapon_sets(controller)) {
    void* item = item_of(wuid);
    if (item && pick(item)) {
      reinterpret_cast<DrawItem>(natives.draw_item)(controller, wuid, kDrawMode);
      return true;
    }
  }
  return false;
}

bool holster(void* actor) {
  if (!actor || !natives.holster_item) {
    return false;
  }
  void* controller = controller_of(actor);
  if (!controller) {
    return false;
  }
  const auto held = hand_items(actor);
  bool held_any = false;
  for (std::size_t hand = 0; hand < held.size(); ++hand) {
    // A two-handed weapon is in both hands: put away once.
    if (!held[hand] || (hand == 1 && held[1] == held[0])) {
      continue;
    }
    reinterpret_cast<HolsterItem>(natives.holster_item)(controller, held[hand]);
    held_any = true;
  }
  return held_any;
}

std::string capture(void* actor) {
  if (!actor || !natives.find_item) {
    owner_actor = nullptr;
    owner_tokens.clear();
    owner_bindings.clear();
    return {};
  }
  if (owner_actor != actor) {
    owner_actor = actor;
    owner_tokens.clear();
    owner_bindings.clear();
    const auto replica = replica_players.find(actor);
    if (replica != replica_players.end()) {
      replicas.erase(replica->second);
      replica_players.erase(replica);
    }
  }
  return kcd2s::integration::hands_field(refresh_owner(actor));
}

std::optional<std::uint32_t> item_token(void* actor, std::uint64_t native_uid) {
  if (!native_uid) {
    return 0;
  }
  if (!actor || !natives.find_item) {
    return std::nullopt;
  }
  if (actor == owner_actor) {
    refresh_owner(actor);
    return owner_bindings.token_for(native_uid, observed_hands(actor));
  }
  const auto player = replica_players.find(actor);
  if (player != replica_players.end()) {
    const auto replica = replicas.find(player->second);
    if (replica != replicas.end() && replica->second.actor == actor) {
      return replica->second.bindings.token_for(native_uid, observed_hands(actor));
    }
  }
  return std::nullopt;
}

std::optional<std::uint64_t> item_from_token(void* actor, std::uint32_t token) {
  if (!token) {
    return 0;
  }
  if (!actor || !natives.find_item) {
    return std::nullopt;
  }
  if (actor == owner_actor) {
    refresh_owner(actor);
    return owner_bindings.item_from(token, observed_hands(actor));
  }
  const auto player = replica_players.find(actor);
  if (player != replica_players.end()) {
    const auto replica = replicas.find(player->second);
    if (replica != replicas.end() && replica->second.actor == actor) {
      return replica->second.bindings.item_from(token, observed_hands(actor));
    }
  }
  return std::nullopt;
}

void observe_replica_items(std::uint32_t player, void* actor, const std::string& field) {
  if (!actor || !natives.find_item) {
    const auto existing = replicas.find(player);
    if (existing != replicas.end()) {
      existing->second.bindings.clear();
    }
    return;
  }
  if (actor == owner_actor) {
    owner_actor = nullptr;
    owner_tokens.clear();
    owner_bindings.clear();
  }
  Replica& replica = replicas[player];
  if (replica.actor != actor) {
    const auto previous = replica_players.find(replica.actor);
    if (previous != replica_players.end() && previous->second == player) {
      replica_players.erase(previous);
    }
    replica = {};
    replica.actor = actor;
  }
  replica_players[actor] = player;
  const bool changed = replica.field != field;
  if (changed) {
    replica.bindings.clear();
    replica.wanted = {};
    replica.valid_field = kcd2s::integration::parse_hands_field(field, replica.wanted);
    replica.field = field;
  } else if (field.empty()) {
    replica.valid_field = true;
  }
  if (!replica.valid_field) {
    replica.bindings.clear();
    if (changed && log_line) {
      log_line("WEAPONS player=%u field refused", player);
    }
    return;
  }
  replica.bindings.bind(replica.wanted, observed_hands(actor));
}

void keep_replica(std::uint32_t player, void* actor, void* entity, const std::string& field) {
  observe_replica_items(player, actor, field);
  const auto existing = replicas.find(player);
  if (!actor || !natives.find_item || existing == replicas.end() ||
      existing->second.actor != actor || !existing->second.valid_field) {
    return;
  }
  Replica& replica = existing->second;
  void* controller = controller_of(actor);
  if (!controller) {
    replica.bindings.clear();
    return;
  }
  const auto sets = weapon_sets(controller);
  const auto held = hand_items(actor);
  if (replica.presentation_initialized && replica.shown_field == field && replica.sets == sets) {
    if (!dropped(replica.held, held)) {
      replica.held = held;
      replica.bindings.bind(replica.wanted, observed_hands(actor));
      return;
    }
    if (log_line) {
      log_line("WEAPONS player=%u replica let go of what it held: drawn again", player);
    }
  }
  replica.shown_field = field;
  replica.presentation_initialized = true;
  replica.sets = sets;
  replica.held = {};
  hold(player, replica, controller, actor, entity, replica.wanted, sets);
  replica.bindings.bind(replica.wanted, observed_hands(actor));
}

void forget(std::uint32_t player) {
  const auto replica = replicas.find(player);
  if (replica != replicas.end()) {
    const auto actor = replica_players.find(replica->second.actor);
    if (actor != replica_players.end() && actor->second == player) {
      replica_players.erase(actor);
    }
  }
  replicas.erase(player);
}

void reset() {
  replicas.clear();
  replica_players.clear();
  owner_actor = nullptr;
  owner_tokens.clear();
  owner_bindings.clear();
}

}  // namespace kcds::runtime::weapons
