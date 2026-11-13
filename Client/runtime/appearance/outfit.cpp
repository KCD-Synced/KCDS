#include "outfit.hpp"

#include "../core/game_layouts.hpp"
#include "MinHook.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace kcds::runtime::appearance::outfit {
namespace {

using ClassId = std::array<std::uint8_t, 16>;

// The item descriptor copies the class id with an aligned 16-byte load.
struct alignas(16) AlignedClassId {
  std::uint8_t bytes[16];
};

using InventoryFind = void*(__fastcall*)(void* inventories, void* entity);
using ItemDescInit = void*(__fastcall*)(void* desc, const AlignedClassId* id, int amount,
                                        float health);
using ItemDescFree = void(__fastcall*)(void* desc);
using InventoryAdd = void*(__fastcall*)(void* inventory, void* desc, unsigned reason, char flag);
using InventoryRemove = void*(__fastcall*)(void* inventory, void* item, unsigned reason,
                                           unsigned amount);
using EquipCall = char(__fastcall*)(void* equipment, void* item, char flag);
using ItemHealthSet = void(__fastcall*)(void* item, float health, void* change);
using ItemQuality = unsigned(__fastcall*)(void* component);

// "KO2": per piece its class id, quality (a byte) and health (two bytes, little-endian, 0 .. 1 in
// 65535 steps). "KO1" (class ids only) is still read.
constexpr char kTag[] = "KO2";
constexpr char kClassesOnlyTag[] = "KO1";
constexpr std::size_t kTagSize = sizeof(kTag) - 1;
constexpr std::size_t kPieceBytes = 16 + 1 + 2;
constexpr float kHealthSteps = 65535.0f;
// An item's health (item + 0x54) and its quality component (item + 0x58, quality at its vtable
// slot 3; answers 1 without one).
constexpr std::size_t kItemHealth = 0x54;
constexpr std::size_t kItemQuality = 0x58;
constexpr std::size_t kQualitySlot = 3;
// The item descriptor's explicit quality (+ 144; 0 lets the game derive it from the health).
constexpr std::size_t kDescQuality = 144;

constexpr std::size_t kActorSoul = kcds::layouts::Actor::kSoul;
constexpr std::size_t kSoulEquipment = kcds::layouts::Soul::kEquipment;
constexpr std::size_t kEquipmentManager = 288;
constexpr std::size_t kWornBegin = 64;
constexpr std::size_t kWornEnd = 72;
// The equipped weapons: item pointers at +152 (melee weapon), +160 (shield), +168 (ranged
// weapon), +176 (ammunition) and the slots after them, null when empty.
constexpr std::size_t kWeaponSlots = 152;
constexpr std::size_t kWeaponSlotCount = 8;
constexpr std::size_t kEquipSlot = 40;
constexpr std::size_t kUnequipSlot = 41;
constexpr std::size_t kItemClass = kcds::layouts::ItemInstance::kClass;
constexpr std::size_t kClassId = 8;
// More pieces than anyone wears, weapons included; a longer list is not a capture.
constexpr std::size_t kMaxPieces = 40;
// Layers of clothing that need the one under them (the equipment slots' body layers 1-4).
constexpr std::size_t kEquipPasses = 4;

// The inventory manager (the book's lookup): *(services + 224) + 232.
constexpr std::size_t kServicesItems = 224;
constexpr std::size_t kItemsInventories = 232;
// Inventory change reasons: 4 is the script CreateItem's, 6 a removal by script.
constexpr unsigned kAddReason = 4;
constexpr unsigned kRemoveReason = 6;

Natives natives;
Logger log_line = nullptr;

// C_Player + 3272: the visor state a helmet's visor toggle updates (null on a replica).
constexpr std::size_t kPlayerVisorSlot = 409;
using VisorUpdate = __int64(__fastcall*)(void** player);
VisorUpdate original_visor_update = nullptr;

__int64 __fastcall guarded_visor_update(void** player) {
  if (!player[kPlayerVisorSlot]) {
    return 0;
  }
  return original_visor_update(player);
}

// The soul an equipment belongs to (equipment + 24).
constexpr std::size_t kEquipmentSoul = 24;
using PresetDue = char(__fastcall*)(void* equipment);
PresetDue original_preset_due = nullptr;

// A piece's quality and health as its owner's game has them; quality 0 and health -1 when the
// owner did not report them (the game then derives both for a created piece).
struct Condition {
  unsigned quality = 0;
  float health = -1.0f;
};

// What a player's replica actor is to wear, and whether its missing pieces were created.
// How often a dressed body is looked at, spread over the crowd.
constexpr ULONGLONG kDressedLookEveryMs = 1000;
constexpr std::uint32_t kDressedLookSpreadMs = 1000;

struct Replica {
  void* actor = nullptr;
  std::string outfit;
  // False while the owner has not reported an outfit: the replica keeps what it wears.
  bool reported = false;
  std::vector<ClassId> wanted;
  std::vector<Condition> conditions;
  bool dressed = false;
  // Whether the worn pieces took the owner's health since the outfit last changed.
  bool conditioned = false;
  // Pieces the replica's equipment refused to put on, and whether it matched its owner.
  unsigned refused = 0;
  bool matched = false;
  // Dressed as the owner is: looked at again only at this time (GetTickCount64), since reading what
  // a body wears is not free and a crowd of them stands dressed.
  ULONGLONG look_again_at = 0;
  // The replica had no soul or inventory yet when the outfit came (logged once).
  bool waiting = false;
  // The times it was dressed again after the game took pieces off it, and the last time.
  unsigned redressed = 0;
  std::chrono::steady_clock::time_point redressed_at{};
};

// A body the game undressed itself (a server horse that became the local player's lost its tack
// on that player's screen, live) is dressed again, at most this often and this many
// times per outfit: a piece the game keeps taking off is not created over and over.
constexpr auto kRedressEvery = std::chrono::seconds(1);
constexpr unsigned kMaxRedresses = 10;
std::unordered_map<std::uint32_t, Replica> replicas;

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

void* soul_of(void* actor) {
  return actor ? read<void*>(actor, kActorSoul) : nullptr;
}

void* equipment_of(void* soul) {
  return static_cast<std::uint8_t*>(soul) + kSoulEquipment;
}

// The items the soul wears, in the game's order, then its equipped weapons.
std::vector<void*> worn_items(void* soul) {
  std::vector<void*> items;
  void* manager = read<void*>(equipment_of(soul), kEquipmentManager);
  if (!manager) {
    return items;
  }
  auto begin = read<void**>(manager, kWornBegin);
  auto end = read<void**>(manager, kWornEnd);
  // An empty vector has no storage: nothing worn, the weapons may still be equipped.
  if (begin && end >= begin && std::size_t(end - begin) <= kMaxPieces) {
    for (auto it = begin; it != end; ++it) {
      if (*it) {
        items.push_back(*it);
      }
    }
  }
  for (std::size_t slot = 0; slot < kWeaponSlotCount; ++slot) {
    if (void* weapon = read<void*>(manager, kWeaponSlots + slot * sizeof(void*))) {
      items.push_back(weapon);
    }
  }
  return items;
}

bool class_of(void* item, ClassId& id) {
  void* klass = read<void*>(item, kItemClass);
  if (!klass) {
    return false;
  }
  std::memcpy(id.data(), static_cast<std::uint8_t*>(klass) + kClassId, id.size());
  return true;
}

// The item's quality as the game answers it.
unsigned quality_of(void* item) {
  void* component = read<void*>(item, kItemQuality);
  if (!component) {
    return 1;
  }
  return slot<ItemQuality>(component, kQualitySlot)(component);
}

// Which of the wanted pieces the worn ones cover (the same class and, when the owner reported
// it, the same quality: the attack reads a weapon's health, and the game keeps health inside its
// quality's band); returns the outermost (last) worn piece that none of them is, or null.
void* match_worn(const std::vector<ClassId>& wanted, const std::vector<Condition>& conditions,
                 const std::vector<void*>& worn, std::vector<bool>& present) {
  present.assign(wanted.size(), false);
  void* extra = nullptr;
  for (void* item : worn) {
    ClassId id;
    bool keep = false;
    if (class_of(item, id)) {
      for (std::size_t index = 0; index < wanted.size(); ++index) {
        const unsigned quality = conditions[index].quality;
        if (!present[index] && wanted[index] == id &&
            (quality == 0 || quality_of(item) == quality)) {
          present[index] = true;
          keep = true;
          break;
        }
      }
    }
    if (!keep) {
      extra = item;
    }
  }
  return extra;
}

// The class ids and conditions of an outfit field; false when it is not one.
bool parse(const std::string& outfit, std::vector<ClassId>& ids,
           std::vector<Condition>& conditions) {
  const bool classes_only =
      outfit.size() >= kTagSize && outfit.compare(0, kTagSize, kClassesOnlyTag) == 0;
  if (outfit.size() < kTagSize || (!classes_only && outfit.compare(0, kTagSize, kTag) != 0)) {
    return false;
  }
  const std::size_t piece_bytes = classes_only ? 16 : kPieceBytes;
  const std::size_t body = outfit.size() - kTagSize;
  if (body % piece_bytes != 0 || body / piece_bytes > kMaxPieces) {
    return false;
  }
  for (std::size_t at = kTagSize; at < outfit.size(); at += piece_bytes) {
    ClassId id;
    std::memcpy(id.data(), outfit.data() + at, id.size());
    ids.push_back(id);
    Condition condition;
    if (!classes_only) {
      condition.quality = std::uint8_t(outfit[at + 16]);
      const unsigned steps =
          unsigned(std::uint8_t(outfit[at + 17])) | unsigned(std::uint8_t(outfit[at + 18])) << 8;
      condition.health = float(steps) / kHealthSteps;
    }
    conditions.push_back(condition);
  }
  return true;
}

void* inventory_of(void* entity) {
  void* services = natives.services ? natives.services() : nullptr;
  void* items = services ? read<void*>(services, kServicesItems) : nullptr;
  void* inventories = items ? read<void*>(items, kItemsInventories) : nullptr;
  if (!inventories || !entity) {
    return nullptr;
  }
  return reinterpret_cast<InventoryFind>(natives.inventory_find)(inventories, entity);
}

// An item of the class in the inventory: with the owner's quality and health when known (the
// descriptor's explicit quality keeps the health as given), else as the game derives them.
void* create_item(void* inventory, const ClassId& id, const Condition& condition = {}) {
  AlignedClassId aligned;
  std::memcpy(aligned.bytes, id.data(), id.size());
  alignas(16) std::uint8_t desc[512] = {};
  reinterpret_cast<ItemDescInit>(natives.item_desc_init)(desc, &aligned, 1, condition.health);
  if (condition.quality > 0) {
    std::memcpy(desc + kDescQuality, &condition.quality, sizeof(condition.quality));
  }
  void* item =
      reinterpret_cast<InventoryAdd>(natives.inventory_add)(inventory, desc, kAddReason, 0);
  reinterpret_cast<ItemDescFree>(natives.item_desc_free)(desc);
  return item;
}

// The replica's worn pieces take their owner's health (the game's own setter),
// once per outfit change.
unsigned take_health(const Replica& replica, const std::vector<void*>& worn) {
  if (!natives.item_health_set) {
    return 0;
  }
  std::vector<bool> used(replica.wanted.size(), false);
  unsigned changed = 0;
  for (void* item : worn) {
    ClassId id;
    if (!class_of(item, id)) {
      continue;
    }
    for (std::size_t index = 0; index < replica.wanted.size(); ++index) {
      const Condition& condition = replica.conditions[index];
      if (used[index] || replica.wanted[index] != id || condition.health < 0) {
        continue;
      }
      used[index] = true;
      if (std::fabs(read<float>(item, kItemHealth) - condition.health) > 0.5f / kHealthSteps) {
        reinterpret_cast<ItemHealthSet>(natives.item_health_set)(item, condition.health, nullptr);
        ++changed;
      }
      break;
    }
  }
  return changed;
}

// True for the soul of a replica this module dresses in its owner's outfit.
bool dressed_by_owner(void* soul) {
  for (const auto& [player, replica] : replicas) {
    if (replica.reported && soul_of(replica.actor) == soul) {
      return true;
    }
  }
  return false;
}

char __fastcall guarded_preset_due(void* equipment) {
  if (dressed_by_owner(read<void*>(equipment, kEquipmentSoul))) {
    return 0;
  }
  return original_preset_due(equipment);
}

}  // namespace

void install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
}

// The visor update's calls: the player's equipment holder (+ 9) and the helmet visor's id
// (+ 30), each an E8 rel32; the player's vtable slot that says the visor is closed.
constexpr std::size_t kVisorHolderCall = 9;
constexpr std::size_t kVisorIdCall = 30;
constexpr std::size_t kPlayerVisorClosed = 184;
using VisorHolder = void*(__fastcall*)(void* player);
using VisorId = void(__fastcall*)(void* holder, std::uint32_t* id);
VisorHolder visor_holder = nullptr;
VisorId visor_id = nullptr;

// The target of the E8 call at `offset` into `function`, or null for another instruction.
void* call_target(void* function, std::size_t offset) {
  const auto* code = static_cast<const std::uint8_t*>(function) + offset;
  if (code[0] != 0xE8) {
    return nullptr;
  }
  std::int32_t relative = 0;
  std::memcpy(&relative, code + 1, sizeof(relative));
  return const_cast<std::uint8_t*>(code) + 5 + relative;
}

bool install_visor_guard(void* visor_update) {
  if (!visor_update) {
    return false;
  }
  visor_holder = reinterpret_cast<VisorHolder>(call_target(visor_update, kVisorHolderCall));
  visor_id = reinterpret_cast<VisorId>(call_target(visor_update, kVisorIdCall));
  return MH_CreateHook(visor_update, reinterpret_cast<void*>(guarded_visor_update),
                       reinterpret_cast<void**>(&original_visor_update)) == MH_OK &&
         MH_EnableHook(visor_update) == MH_OK;
}

bool visor_closed(void* player) {
  if (!player || !visor_holder || !visor_id || !static_cast<void**>(player)[kPlayerVisorSlot]) {
    return false;
  }
  void* holder = visor_holder(player);
  if (!holder) {
    return false;
  }
  std::uint32_t visor = 0;
  visor_id(holder, &visor);
  if (visor == 0) {
    return false;
  }
  using IsClosed = bool(__fastcall*)(void* player);
  return reinterpret_cast<IsClosed>((*static_cast<void***>(player))[kPlayerVisorClosed])(player);
}

constexpr std::size_t kActorRaiseVisor = 182;
constexpr std::size_t kActorCloseVisor = 183;

void set_visor(void* actor, bool closed) {
  if (!actor) {
    return;
  }
  using Toggle = void(__fastcall*)(void* actor);
  const std::size_t slot = closed ? kActorCloseVisor : kActorRaiseVisor;
  reinterpret_cast<Toggle>((*static_cast<void***>(actor))[slot])(actor);
}

bool install_preset_guard(void* preset_due) {
  if (!preset_due) {
    return false;
  }
  return MH_CreateHook(preset_due, reinterpret_cast<void*>(guarded_preset_due),
                       reinterpret_cast<void**>(&original_preset_due)) == MH_OK &&
         MH_EnableHook(preset_due) == MH_OK;
}

std::string capture(void* actor) {
  void* soul = soul_of(actor);
  if (!soul) {
    return {};
  }
  std::string outfit = kTag;
  for (void* item : worn_items(soul)) {
    ClassId id;
    if (!class_of(item, id)) {
      continue;
    }
    outfit.append(reinterpret_cast<const char*>(id.data()), id.size());
    const unsigned quality = quality_of(item);
    outfit.push_back(char(quality > 255 ? 255 : quality));
    float health = read<float>(item, kItemHealth);
    health = std::isfinite(health) ? std::clamp(health, 0.0f, 1.0f) : 1.0f;
    const unsigned steps = unsigned(std::lround(health * kHealthSteps));
    outfit.push_back(char(steps & 0xFF));
    outfit.push_back(char(steps >> 8));
  }
  return outfit;
}

void apply(std::uint32_t player, void* actor, void* entity, const std::string& outfit) {
  Replica& replica = replicas[player];
  if (replica.actor != actor || replica.outfit != outfit) {
    replica = Replica{};
    replica.actor = actor;
    replica.outfit = outfit;
    replica.reported = parse(outfit, replica.wanted, replica.conditions);
    if (log_line && replica.reported) {
      log_line("APPEARANCE outfit player=%u received pieces=%zu", player, replica.wanted.size());
    }
  }
  if (!replica.reported) {
    return;
  }
  const ULONGLONG look_now = GetTickCount64();
  if (look_now < replica.look_again_at) {
    return;
  }
  void* soul = soul_of(actor);
  void* inventory = inventory_of(entity);
  if (!soul || !inventory || !natives.inventory_add || !natives.inventory_remove) {
    if (log_line && !replica.waiting) {
      replica.waiting = true;
      log_line("APPEARANCE outfit player=%u waits soul=%p inventory=%p", player, soul, inventory);
    }
    return;
  }
  void* equipment = equipment_of(soul);
  // Off and out with every piece the owner does not wear, also one the game puts on the replica
  // later (its default clothing arrives over several frames); what the owner wears stays on. One
  // piece at a time, the outermost (last put on) first, reading what is worn again after each:
  // taking a piece off can take others with it (a padded coif its helmet), and a piece read
  // before may no longer be worn.
  std::vector<bool> present;
  unsigned removed = 0;
  for (std::size_t step = 0; step <= kMaxPieces; ++step) {
    void* extra = match_worn(replica.wanted, replica.conditions, worn_items(soul), present);
    if (!extra) {
      break;
    }
    slot<EquipCall>(equipment, kUnequipSlot)(equipment, extra, 1);
    reinterpret_cast<InventoryRemove>(natives.inventory_remove)(inventory, extra, kRemoveReason, 0);
    ++removed;
  }
  // A piece taken off that the owner still wears (another quality) is created again at once with
  // the owner's quality and health (a replica's sword went and stayed gone).
  const bool missing = std::find(present.begin(), present.end(), false) != present.end();
  if (removed && missing) {
    replica.dressed = false;
  }
  // Dressed with every piece on, and now the game took some off by itself.
  const auto now = std::chrono::steady_clock::now();
  if (replica.dressed && !removed && missing && replica.refused == 0 &&
      replica.redressed < kMaxRedresses && now - replica.redressed_at >= kRedressEvery) {
    replica.dressed = false;
    ++replica.redressed;
    replica.redressed_at = now;
    if (log_line) {
      log_line("APPEARANCE outfit player=%u lost pieces the game took off: dressed again (%u)",
               player, replica.redressed);
    }
  }
  // The missing pieces are created once per outfit and put on in passes: a piece that needs
  // another one under it (a helmet its padded coif, mail its gambeson) goes on once that one is.
  unsigned added = 0;
  if (!replica.dressed) {
    std::vector<void*> pending;
    for (std::size_t index = 0; index < replica.wanted.size(); ++index) {
      if (present[index]) {
        continue;
      }
      if (void* item = create_item(inventory, replica.wanted[index], replica.conditions[index])) {
        pending.push_back(item);
      }
    }
    for (std::size_t pass = 0; pass < kEquipPasses && !pending.empty(); ++pass) {
      std::vector<void*> refused;
      for (void* item : pending) {
        if (slot<EquipCall>(equipment, kEquipSlot)(equipment, item, 1)) {
          ++added;
        } else {
          refused.push_back(item);
        }
      }
      if (refused.size() == pending.size()) {
        break;
      }
      pending = std::move(refused);
    }
    replica.dressed = true;
    replica.refused = unsigned(pending.size());
  }
  if (log_line && (removed || added)) {
    log_line("APPEARANCE outfit player=%u pieces=%zu removed=%u added=%u refused=%u", player,
             replica.wanted.size(), removed, added, replica.refused);
  }
  // Once the replica wears exactly the owner's pieces, say so (the live tests read it).
  const bool all_present = std::all_of(present.begin(), present.end(), [](bool on) { return on; });
  if (removed == 0 && added == 0 && all_present) {
    replica.look_again_at = look_now + kDressedLookEveryMs + player % kDressedLookSpreadMs;
  }
  if (log_line && !replica.matched && removed == 0 && all_present) {
    replica.matched = true;
    log_line("APPEARANCE outfit player=%u matches pieces=%zu", player, replica.wanted.size());
  }
  // Then the pieces take the owner's health, once per outfit (a weapon wears in use: the attack
  // the replica deals reads it).
  if (removed == 0 && all_present && !replica.conditioned) {
    replica.conditioned = true;
    const unsigned changed = take_health(replica, worn_items(soul));
    if (log_line && changed) {
      log_line("APPEARANCE outfit player=%u health taken pieces=%u", player, changed);
    }
  }
}

std::vector<void*> worn(void* actor) {
  void* soul = soul_of(actor);
  return soul ? worn_items(soul) : std::vector<void*>{};
}

bool wear(void* actor, void* item, bool on) {
  void* soul = soul_of(actor);
  if (!soul || !item) {
    return false;
  }
  void* equipment = equipment_of(soul);
  return slot<EquipCall>(equipment, on ? kEquipSlot : kUnequipSlot)(equipment, item, 1) != 0;
}

std::string describe(const std::string& outfit) {
  std::vector<ClassId> ids;
  std::vector<Condition> conditions;
  if (!parse(outfit, ids, conditions)) {
    return "pieces=none";
  }
  std::string text = "pieces=" + std::to_string(ids.size());
  for (const ClassId& id : ids) {
    // The class id's first field, little-endian, as the tables' "01151dab-..." starts.
    char part[12];
    snprintf(part, sizeof(part), " %02x%02x%02x%02x", id[3], id[2], id[1], id[0]);
    text += part;
  }
  return text;
}

void* create_item_in_inventory(void* entity, const std::array<std::uint8_t, 16>& class_id) {
  void* inventory = inventory_of(entity);
  if (!inventory || !natives.inventory_add) {
    return nullptr;
  }
  return create_item(inventory, class_id);
}

void forget(std::uint32_t player) {
  replicas.erase(player);
}

}  // namespace kcds::runtime::appearance::outfit
