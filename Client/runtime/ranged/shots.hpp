// Shots with bows and crossbows seen by everyone, WHGame 1.5.5.
//
// The game fires a projectile (context, shooter, kind, direction, slot info,
// position, weapon item, ammunition item, speed, flag): it takes a projectile for the
// ammunition (which moves one unit of the ammunition item into it) and launches it with
// CProjectile::Launch (projectile, position, direction, velocity, speed scale). The
// context is the game's projectile owner registry; the shooter's weapon is its shooting
// expansion's (the actor expansion of type 4, + 64).
//
// The shooter's client reports each projectile its player fires: the ammunition's class, the
// position and direction it is launched with, its speed and kind. Every other client fires the same
// projectile from the shooter's replica, with the replica's weapon and a unit of that ammunition
// made for it, so it flies the same and the game of the player it hits works the hit out.
//
// Engine thread.
#ifndef KCDS_RUNTIME_RANGED_SHOTS_HPP
#define KCDS_RUNTIME_RANGED_SHOTS_HPP

#include <array>
#include <cstdint>
#include <vector>

#include "kcd2s/integration/shots.hpp"

namespace kcds::runtime::ranged {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* fire = nullptr;
  void* launch = nullptr;
  void* projectile_owners = nullptr;   // the context getter
  void* shooting_expansion = nullptr;  // (the actor's expansions, actor + 640)
  // An item of a class made in an entity's inventory (appearance::outfit's).
  void* (*create_item)(void* entity, const std::array<std::uint8_t, 16>& class_id) = nullptr;
  // The item in an actor's hand (weapons::item_in_hand), the weapon when the expansion has none.
  void* (*item_in_hand)(void* actor, bool left) = nullptr;
  void* (*local_actor)() = nullptr;
  // A weapon item's missile weapon, null for another item.
  void* missile_weapon = nullptr;
  // The game's release of a shot on the shooter's shooting expansion: takes the
  // arrow or bolt off its weapon's string.
  void* released = nullptr;
  // Whether a missile weapon shows a bolt (or a gun its load) ready: missile weapon + 216, set by
  // (missile weapon, loaded), which shows or hides it.
  void* set_loaded = nullptr;
  // The game's load of a bow when its string is empty (unused, actor): its missile
  // weapon takes the shooting expansion's ammunition (+ 72), which puts an arrow made
  // of it on the bow's "arrow" attachment. And the unload (missile weapon).
  void* load_ammunition = nullptr;
  void* unload_ammunition = nullptr;
};

// False when a native is missing or a hook fails.
bool install(const Natives& natives, Logger logger);

// The projectiles the local player fired since the last call, oldest first.
std::vector<kcd2s::integration::Shot> take_shots();

// Fires another player's shot from their replica (its actor and engine entity); false when the
// replica holds no weapon or the ammunition could not be made.
bool replay(void* actor, void* entity, const kcd2s::integration::Shot& shot);

// Whether an item is a bow, crossbow or gun (it has a missile weapon).
bool is_missile_weapon(void* item);

// The state of the bow, crossbow or gun an actor holds: kWeaponLoaded (a bolt on the string, a
// gun's load), kWeaponReady (drawn and ready to shoot); 0 without one.
inline constexpr std::uint8_t kWeaponLoaded = 1;
inline constexpr std::uint8_t kWeaponReady = 2;
// An arrow on the bow's string: the missile weapon's loaded ammunition (+ 224) is set.
inline constexpr std::uint8_t kWeaponNocked = 4;
std::uint8_t weapon_state(void* actor);

// A replica's weapon takes its owner's state: the bolt on the string shows and leaves with
// the shot, and the game's own hand props (the arrow from the quiver and back, a gun's ramrod
// and match) follow the replica's clips. Its weapon stays ready for a moment after the owner's
// stopped being ready, so the replica's clips (a moment behind) still put the arrow away. A bow
// whose owner has an arrow on the string is loaded by the game's own load when its string is
// empty (the replica's reload clip loaded nothing, its bow stayed empty after the first
// shot), and unloaded by the game's unload once its owner put the bow down without a shot.
void keep_weapon_state(void* actor, std::uint8_t state);

}  // namespace kcds::runtime::ranged

#endif  // KCDS_RUNTIME_RANGED_SHOTS_HPP
