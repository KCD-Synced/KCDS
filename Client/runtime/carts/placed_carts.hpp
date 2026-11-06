// The carts and wagons the server places (kcd2s/integration/carts.hpp).
//
// A cart is built from the parts of the game's prefab of its blueprint (Data/Prefabs/manmade/
// vehicles: the axle, the body and the wheels as static meshes): the first part is the cart's
// root entity, named after the cart's server id, and the others hang from it (AttachChild), so
// one move of the root moves the whole cart. The game's own cart system (C_Cart and its AI
// teamster, paths and smart objects) is not used: it drives NPC carts along level paths and has
// no player driver.
//
// A harnessed cart has its horses in front (cart_horses.hpp: every client spawns its own, where
// the game's wagon character puts them, its bones animal_horse_L and animal_horse_R), walking as
// the cart goes. Each client sets the cart on the ground under its axles (its height and pitch).
//
// Authority is the driver's, as a horse's is its simulator's (riding/native_horses): the client of
// the player in the `driver` seat runs the cart. The driver of a wagon with a front plank sits
// on it and drives with the movement actions (take_action: forward and back, left and right, the
// sprint for a trot); the driver of a c wagon or a cart walks beside the horse's head, and of a
// cart without horses at its hitch, and leads it on foot: that point follows the driver and the
// cart turns towards them as a towed cart does. That
// client reports the pose and pace (CART_REPORT Drive) when they changed, at most ten times a
// second; every other client moves the cart towards the server's pose (CART Pose). A seated
// player's own client holds them on the seat, facing its way; the use key gets them down
// (CART_REPORT Leave). The other clients see that player's replica there.
//
// Engine thread.
#pragma once

#include <cstdint>

#include "kcd2s/integration/carts.hpp"

namespace kcds::runtime::carts::placed {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* attach_child = nullptr;  // CEntity::AttachChild(parent, child, params)
  // The local player's network id (0 before it has one).
  std::uint32_t (*local_player)() = nullptr;
  // The local player's body: its world position and rotation (x, y, z, w); false without one.
  bool (*local_pose)(float position[3], float rotation[4]) = nullptr;
  // Puts the local player's body at a world position and rotation (null keeps its rotation).
  void (*place_local)(const float position[3], const float rotation[4]) = nullptr;
  // Presses a game action on the local player's input, as a key would (mode 1 press, 2 release; for
  // rotateyaw the value is degrees to the right). Without it a player is put at their climb's
  // start.
  void (*press)(const char* action, unsigned mode, float value) = nullptr;
  // Sends a report to the server; false when it could not.
  bool (*report)(const kcd2s::integration::CartReport& report) = nullptr;
  // Keeps the cart horse of that name (spawned when missing) at the world position and rotation,
  // walking with the velocity (metres a second) and turn (radians a second, to the left); false
  // while it could not be spawned. Without it no horse is shown.
  // side: 0 a horse alone, 1 the left one of a pair, 2 the right one (its harness); cart_root:
  // the cart's root entity's name; driver: who holds the reins, a player's network id (0 for none);
  // reins_rest: where the free reins' two ends lie, in the world (right hand's, left hand's).
  bool (*keep_horse)(const char* name, const float position[3], const float rotation[4],
                     const float velocity[3], float turn_rate, int side, const char* cart_root,
                     std::uint32_t driver, const float reins_rest[6]) = nullptr;
  void (*remove_horse)(const char* name) = nullptr;
  // The ground's height under a world point, looked for from `above` down; false when none.
  // Without it the carts keep the height the server gives.
  bool (*ground_height)(float x, float y, float above, float& z) = nullptr;
  // True when something solid (static objects and rigid bodies; not the terrain nor the living)
  // lies between the two world points. Without it the carts are not stopped by anything.
  // Something solid between the points, not the terrain and not the parts of cart `own`.
  bool (*blocked)(const float from[3], const float to[3], std::uint32_t own) = nullptr;
  Logger log = nullptr;
};

// False when a native is missing; no cart is then shown.
bool install(const Natives& natives);

// Spawns, moves, seats or removes a cart.
void apply(const kcd2s::integration::CartOrder& order);

// Every frame: the driver's cart follows the driver and is reported, the other carts move
// towards the server's pose, and a passenger is held on the seat. `now` in seconds.
void frame(double now);

// Forgets every cart, as the game's level does when it unloads.
void reset();

// The local player is held on a seat: only their head and view turn, the body faces the seat's way.
bool local_held();

// A player is on a cart's seat or on their way up or down it: their capsule is off.
bool rider_on_cart(std::uint32_t player);

// Where a player seated on a cart sits in this game: their cart's pose as this game shows it, plus
// the seat. False for a player on no seat or climbing on or down (their own reports place them).
// `position` and `rotation` hold where their replica stands now; it comes to rest on the seat. A
// rider's own reports come from their own game's cart, which is ahead of this game's: placed by
// them, a rider slid along the moving cart.
bool rider_pose(std::uint32_t player, float position[3], float rotation[4]);

// The local player's input action (the player input's OnAction; mode 1 pressed, 2 released):
// true when a seated driver takes it to drive (moveforward, moveback, moveleft, moveright,
// sprint), so the body does not walk, a held player's use key to get down, or a held player's
// turning, which would turn the seated body.
bool take_action(const char* action, unsigned mode);

}  // namespace kcds::runtime::carts::placed
