#include "shots.hpp"

#include <windows.h>

#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"
#include "landings.hpp"

namespace kcds::runtime::ranged {
namespace {

using kcd2s::integration::Shot;

using Fire = std::intptr_t(__fastcall*)(void* owners, void* shooter, int kind, float* direction,
                                        void* slot_info, float* position, void* weapon,
                                        void* ammunition, float speed, char flag);
using Launch = std::intptr_t(__fastcall*)(void* projectile, const float* position,
                                          const float* direction, const float* velocity,
                                          float scale);
using ProjectileOwners = void*(__fastcall*)();
using ShootingExpansion = void*(__fastcall*)(void* expansions);
using MissileWeapon = void*(__fastcall*)(void* weapon);
using Released = void(__fastcall*)(void* shooting_expansion);
using SetLoaded = void(__fastcall*)(void* missile_weapon, char loaded);
using LoadAmmunition = void*(__fastcall*)(void* unused, void* actor);
using UnloadAmmunition = void*(__fastcall*)(void* missile_weapon);
// A missile weapon's loaded ammunition (the item's WUID, 0 for none).
constexpr std::size_t kMissileAmmunition = 224;
// A missile weapon's loaded state (set_loaded); a shooting expansion's ready state (the weapon
// drawn and ready to shoot).
constexpr std::size_t kMissileLoaded = 216;
constexpr std::size_t kExpansionReady = 89;
// How long a replica's weapon stays ready after its owner's stopped being ready, in ms: the
// replica's clips run a moment behind the owner's, and their hand-prop events (the arrow put on
// the string or back into the quiver) act only on a ready weapon.
constexpr ULONGLONG kReadyHoldMs = 2000;

// The actor's expansions; the shooting expansion's weapon and ammunition item.
constexpr std::size_t kActorExpansions = 640;
constexpr std::size_t kExpansionWeapon = 64;
constexpr std::size_t kExpansionAmmunition = 72;
// An item's class and the class's GUID.
constexpr std::size_t kItemClass = 0x48;
constexpr std::size_t kClassGuid = 8;
constexpr std::size_t kMaxShots = 16;

Natives natives;
Logger log_line = nullptr;
Fire original_fire = nullptr;
Launch original_launch = nullptr;
// A shot of the local player being fired: what fire knows, then where launch sends it.
bool firing_local = false;
bool launched = false;
Shot firing;
std::vector<Shot> shots;
// A replayed shot is never reported.
bool replaying = false;

template <class Value>
Value at(const void* object, std::size_t offset) {
  Value value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

std::intptr_t __fastcall fire(void* owners, void* shooter, int kind, float* direction,
                              void* slot_info, float* position, void* weapon, void* ammunition,
                              float speed, char flag) {
  const bool local =
      !replaying && natives.local_actor && shooter && shooter == natives.local_actor();
  void* ammunition_class = ammunition ? at<void*>(ammunition, kItemClass) : nullptr;
  if (!local || !ammunition_class) {
    return original_fire(owners, shooter, kind, direction, slot_info, position, weapon, ammunition,
                         speed, flag);
  }
  firing = Shot{};
  firing.ammo = at<std::array<std::uint8_t, 16>>(ammunition_class, kClassGuid);
  firing.speed = speed;
  firing.kind = std::uint8_t(kind);
  firing_local = true;
  launched = false;
  const std::intptr_t result = original_fire(owners, shooter, kind, direction, slot_info, position,
                                             weapon, ammunition, speed, flag);
  firing_local = false;
  if (launched && shots.size() < kMaxShots) {
    shots.push_back(firing);
  }
  if (launched) {
    expect_landing(true, firing.ammo, firing.position);
    if (log_line) {
      // DIAG: where the local player's projectile leaves and points.
      log_line("SHOT local launch pos=(%.2f %.2f %.2f) dir=(%.3f %.3f %.3f) speed=%.1f kind=%d",
               firing.position[0], firing.position[1], firing.position[2], firing.direction[0],
               firing.direction[1], firing.direction[2], firing.speed, firing.kind);
    }
  }
  return result;
}

std::intptr_t __fastcall launch(void* projectile, const float* position, const float* direction,
                                const float* velocity, float scale) {
  if (firing_local && position && direction) {
    std::memcpy(firing.position, position, sizeof(firing.position));
    std::memcpy(firing.direction, direction, sizeof(firing.direction));
    launched = true;
  }
  return original_launch(projectile, position, direction, velocity, scale);
}

bool hook(void* target, void* detour, void** original) {
  return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.fire || !natives.launch || !natives.projectile_owners ||
      !natives.shooting_expansion || !natives.create_item || !natives.item_in_hand ||
      !natives.local_actor) {
    return false;
  }
  return hook(natives.fire, reinterpret_cast<void*>(fire),
              reinterpret_cast<void**>(&original_fire)) &&
         hook(natives.launch, reinterpret_cast<void*>(launch),
              reinterpret_cast<void**>(&original_launch));
}

std::vector<Shot> take_shots() {
  return std::exchange(shots, {});
}

// The weapon an actor holds: its shooting expansion's, else the bow in its left hand (a replica's
// hands are drawn by weapons/), else whatever its right hand holds (a crossbow).
void* held_weapon(void* actor) {
  void* expansions = at<void*>(actor, kActorExpansions);
  void* expansion =
      expansions ? reinterpret_cast<ShootingExpansion>(natives.shooting_expansion)(expansions)
                 : nullptr;
  void* weapon = expansion ? at<void*>(expansion, kExpansionWeapon) : nullptr;
  if (!weapon) {
    weapon = natives.item_in_hand(actor, true);
  }
  if (!weapon) {
    weapon = natives.item_in_hand(actor, false);
  }
  return weapon;
}

// The missile weapon of the bow or crossbow an actor holds, null for none.
void* held_missile_weapon(void* actor) {
  if (!actor || !natives.missile_weapon) {
    return nullptr;
  }
  void* weapon = held_weapon(actor);
  return weapon ? reinterpret_cast<MissileWeapon>(natives.missile_weapon)(weapon) : nullptr;
}

// Replicas that fired a shot and have not yet seen their owner's string empty: the owner's
// arrow may still read as on the string for a moment (its state comes apart from the shot), and
// the replica loads the next one only after its owner's reload.
std::unordered_set<void*> awaiting_empty_string;

bool is_missile_weapon(void* item) {
  return item && natives.missile_weapon &&
         reinterpret_cast<MissileWeapon>(natives.missile_weapon)(item) != nullptr;
}

bool replay(void* actor, void* entity, const Shot& shot) {
  if (!original_fire || !actor || !entity) {
    return false;
  }
  awaiting_empty_string.insert(actor);
  void* weapon = held_weapon(actor);
  if (!weapon) {
    if (log_line) {
      log_line("SHOT replica %p holds no weapon: not fired", actor);
    }
    return false;
  }
  // One unit of the ammunition, which the shot takes.
  void* ammunition = natives.create_item(entity, shot.ammo);
  if (!ammunition) {
    if (log_line) {
      log_line("SHOT replica %p: the ammunition could not be made", actor);
    }
    return false;
  }
  float position[3];
  float direction[3];
  std::memcpy(position, shot.position, sizeof(position));
  std::memcpy(direction, shot.direction, sizeof(direction));
  // No slot info: the position is where the owner's projectile left.
  alignas(16) std::uint8_t slot_info[16] = {};
  void* owners = reinterpret_cast<ProjectileOwners>(natives.projectile_owners)();
  replaying = true;
  original_fire(owners, actor, shot.kind, direction, slot_info, position, weapon, ammunition,
                shot.speed, 1);
  replaying = false;
  // The game's own release takes the arrow or bolt off the replica's string (its owner's release
  // did on its own client); without it the replica's bow kept one after the shot.
  void* expansions = at<void*>(actor, kActorExpansions);
  void* expansion =
      expansions ? reinterpret_cast<ShootingExpansion>(natives.shooting_expansion)(expansions)
                 : nullptr;
  if (expansion && natives.released && held_missile_weapon(actor) &&
      at<void*>(expansion, kExpansionWeapon) == weapon) {
    reinterpret_cast<Released>(natives.released)(expansion);
  }
  expect_landing(false, shot.ammo, shot.position);
  return true;
}

// The shooting expansion of an actor, null for none.
void* shooting_expansion_of(void* actor) {
  void* expansions = actor ? at<void*>(actor, kActorExpansions) : nullptr;
  return expansions ? reinterpret_cast<ShootingExpansion>(natives.shooting_expansion)(expansions)
                    : nullptr;
}

std::uint8_t weapon_state(void* actor) {
  std::uint8_t state = 0;
  void* missile = held_missile_weapon(actor);
  if (missile && at<std::uint8_t>(missile, kMissileLoaded) != 0) {
    state |= kWeaponLoaded;
  }
  void* expansion = shooting_expansion_of(actor);
  if (missile && expansion && at<std::uint8_t>(expansion, kExpansionReady) != 0) {
    state |= kWeaponReady;
  }
  if (missile && at<std::uint64_t>(missile, kMissileAmmunition) != 0) {
    state |= kWeaponNocked;
  }
  return state;
}

// Replicas whose owner's weapon stopped being ready: when their own weapon stops being ready.
std::unordered_map<void*, ULONGLONG> ready_until;
// Replicas already logged as holding no bow, crossbow or gun.
std::unordered_set<void*> logged_unarmed;

void keep_weapon_state(void* actor, std::uint8_t state) {
  void* missile = held_missile_weapon(actor);
  void* expansion = shooting_expansion_of(actor);
  if (!missile || !expansion) {
    if (state != 0 && log_line && logged_unarmed.insert(actor).second) {
      log_line("SHOT replica %p state %u, but no %s", actor, unsigned(state),
               missile ? "shooting expansion" : "missile weapon in hand");
    }
    ready_until.erase(actor);
    return;
  }
  logged_unarmed.erase(actor);
  // Ready: the game's hand-prop handlers (the arrow taken from the quiver and put back, a gun's
  // ramrod and match: AttachAmmo / DetachAmmo) act only on a ready shooting expansion,
  // which the replica's missile controller (it takes no input) never makes ready.
  auto& ready =
      *reinterpret_cast<std::uint8_t*>(static_cast<std::uint8_t*>(expansion) + kExpansionReady);
  bool wanted_ready = (state & kWeaponReady) != 0;
  if (wanted_ready) {
    ready_until.erase(actor);
  } else if (ready != 0) {
    const ULONGLONG now = GetTickCount64();
    const auto hold = ready_until.try_emplace(actor, now + kReadyHoldMs).first;
    wanted_ready = now < hold->second;
    if (!wanted_ready) {
      ready_until.erase(hold);
    }
  }
  if ((ready != 0) != wanted_ready) {
    ready = wanted_ready ? 1 : 0;
    if (log_line) {
      log_line("SHOT replica %p weapon %s", actor, wanted_ready ? "ready" : "not ready");
    }
  }
  const bool loaded = (state & kWeaponLoaded) != 0;
  if (natives.set_loaded && (at<std::uint8_t>(missile, kMissileLoaded) != 0) != loaded) {
    reinterpret_cast<SetLoaded>(natives.set_loaded)(missile, loaded ? 1 : 0);
    if (log_line) {
      log_line("SHOT replica %p weapon %s", actor, loaded ? "loaded" : "empty");
    }
  }
  // The arrow on the string: the game's own load or unload, as the owner's bow has it.
  const bool nocked = (state & kWeaponNocked) != 0;
  const bool replica_nocked = at<std::uint64_t>(missile, kMissileAmmunition) != 0;
  if (!nocked) {
    awaiting_empty_string.erase(actor);
  }
  if (nocked && !replica_nocked && wanted_ready && !awaiting_empty_string.count(actor) &&
      natives.load_ammunition && at<void*>(expansion, kExpansionAmmunition)) {
    reinterpret_cast<LoadAmmunition>(natives.load_ammunition)(nullptr, actor);
    if (log_line) {
      log_line("SHOT replica %p arrow put on the string (%s)", actor,
               at<std::uint64_t>(missile, kMissileAmmunition) != 0 ? "loaded" : "not loaded");
    }
  } else if (!nocked && replica_nocked && !wanted_ready && natives.unload_ammunition) {
    reinterpret_cast<UnloadAmmunition>(natives.unload_ammunition)(missile);
    if (log_line) {
      log_line("SHOT replica %p arrow taken off the string", actor);
    }
  }
}

}  // namespace kcds::runtime::ranged
