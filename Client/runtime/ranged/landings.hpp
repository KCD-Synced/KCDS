// Projectiles lying where they landed, WHGame 1.5.5.
//
// A projectile that lands whole becomes a pickable (an entity of the PickableItem class laying an
// item of the ammunition's class, made where it lies). Each client has its own copy of every shot,
// so each would have its own arrow to take up. The shooter's client reports where its own lands
// (SHOT_LANDED) and the server lays it as a ground stack of its own, which every client lays, one
// player takes up and a client that comes later sees; every client takes its own copy (of its own
// shot or of a shot it fired for a replica) out of its world.
//
// A pickable is a shot's when it is made within kFlight of a shot of that ammunition, near where
// the shot left; each shot claims one. A shot whose projectile broke or struck a body claims none.
//
// Engine thread, except pickable_spawned.
#ifndef KCDS_RUNTIME_RANGED_LANDINGS_HPP
#define KCDS_RUNTIME_RANGED_LANDINGS_HPP

#include <array>
#include <cstdint>
#include <vector>

#include "kcd2s/integration/shots.hpp"

namespace kcds::runtime::ranged {

using LandingLogger = void (*)(const char* format, ...);

struct LandingNatives {
  // An entity's world matrix (3 x 4, rows; the position in the last column); false when there is
  // no such entity.
  bool (*entity_transform)(std::uint32_t entity, float* matrix) = nullptr;
  // The item a pickable entity lays loose in the world (not a server stack), or null.
  void* (*level_item_of)(std::uint32_t entity) = nullptr;
  // Removes an entity.
  void (*remove_entity)(std::uint32_t entity) = nullptr;
  // The type of an entity's physics (the engine's pe_type: 2 a rigid body); 0 when it has none,
  // -1 when there is no such entity.
  int (*physics_type)(std::uint32_t entity) = nullptr;
};

// False when a native is missing.
bool install_landings(const LandingNatives& natives, LandingLogger logger);

// A shot was fired on this client: the local player's own (`local`) or another player's from
// their replica. Its projectile may land.
void expect_landing(bool local, const std::array<std::uint8_t, 16>& ammo, const float* origin);

// An entity of the pickable class was made. Any thread.
void pickable_spawned(std::uint32_t entity);

// Looks at the pickables made since the last call that are old enough: a shot's is taken out of
// the world, and the local player's own is kept for take_landings. Call every frame.
void tick_landings();

// Where the local player's projectiles landed since the last call, for the server.
std::vector<kcd2s::integration::ShotLanded> take_landings();

// The world changed: the shots in flight and the pickables waiting go.
void reset_landings();

}  // namespace kcds::runtime::ranged

#endif  // KCDS_RUNTIME_RANGED_LANDINGS_HPP
