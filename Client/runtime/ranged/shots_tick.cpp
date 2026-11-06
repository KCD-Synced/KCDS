#include "shots_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../npcs/native_npcs.hpp"
#include "../replicas/remote.hpp"
#include "../weapons/drawn_weapons.hpp"
#include "../physics/line_of_sight.hpp"
#include "../physics/world_raycast.hpp"
#include "landings.hpp"
#include "shots.hpp"

#include <utility>
#include <vector>

namespace kcds::game_client {

namespace {

// What a landed projectile struck (playerMissileHit): a ray of kLandingReach
// either way along its shaft (its +Y turned by its rotation) through where it lies, else one down
// through it for one lying flat; the first hit described as World.raycast describes one.
constexpr float kLandingReach = 0.5f;

void describe_landing(kcd2s::integration::ShotLanded& landed) {
  const float* q = landed.rotation;
  // +Y turned by the quaternion (x, y, z, w).
  const float axis[3] = {2 * (q[0] * q[1] - q[3] * q[2]), 1 - 2 * (q[0] * q[0] + q[2] * q[2]),
                         2 * (q[1] * q[2] + q[3] * q[0])};
  float from[3];
  float to[3];
  for (int i = 0; i < 3; ++i) {
    from[i] = landed.position[i] - axis[i] * kLandingReach;
    to[i] = landed.position[i] + axis[i] * kLandingReach;
  }
  kcds::runtime::physics::RayHit hit;
  auto result =
      kcds::runtime::physics::trace(from, to, kcds::runtime::physics::kHitCover, nullptr, 0, hit);
  // One lying flat misses along its shaft: what is under it, then.
  if (result != kcds::runtime::physics::TraceResult::Hit) {
    const float above[3] = {landed.position[0], landed.position[1],
                            landed.position[2] + kLandingReach};
    const float below[3] = {landed.position[0], landed.position[1],
                            landed.position[2] - kLandingReach};
    result = kcds::runtime::physics::trace(above, below, kcds::runtime::physics::kHitCover, nullptr,
                                           0, hit);
  }
  if (result == kcds::runtime::physics::TraceResult::Hit) {
    landed.hit = kcds::runtime::physics::server_hit(hit, hit.distance);
  }
}

}  // namespace

// Shots with bows and crossbows: the local player's go to the server, the other
// players' are fired from their replicas.
void tick_shots(void* entity_system) {
  std::vector<std::pair<std::uint32_t, kcd2s::integration::Shot>> relays;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    for (const auto& shot : kcds::runtime::ranged::take_shots()) {
      client->peer().send_shot(shot);
    }
    relays = client->peer().take_shot_relays();
  }
  // Where the projectiles landed: the local player's own go to the server, which lays them for
  // everyone; every copy this client's game made goes.
  kcds::runtime::ranged::tick_landings();
  auto landings = kcds::runtime::ranged::take_landings();
  for (auto& landed : landings) {
    describe_landing(landed);
  }
  if (!landings.empty()) {
    std::lock_guard<std::mutex> lock(network_mutex);
    for (const auto& landed : landings) {
      client->peer().send_shot_landed(landed);
    }
  }
  for (const auto& [shooter, shot] : relays) {
    // The shooter is another player, or a server NPC whose bow its simulator fired.
    const auto remote = remotes.find(shooter);
    void* actor = nullptr;
    std::uint32_t engine = 0;
    if (remote != remotes.end()) {
      actor = remote->second.actor;
      engine = remote->second.engine_id;
    } else {
      engine = engine_body_of(shooter, actor);
      // An NPC replica holding nothing yet takes its bow up for the shot (its armed state's draw
      // may come later).
      if (actor && !kcds::runtime::weapons::item_in_hand(actor, false) &&
          !kcds::runtime::weapons::item_in_hand(actor, true)) {
        kcds::runtime::weapons::draw_set_item(actor, kcds::runtime::ranged::is_missile_weapon);
      }
    }
    void* entity = engine && entity_system ? get_entity(entity_system, engine) : nullptr;
    const bool fired = entity && actor && kcds::runtime::ranged::replay(actor, entity, shot);
    log("SHOT of %s %u at %.2f %.2f %.2f speed %.1f: %s",
        remote != remotes.end() ? "player" : "npc", shooter, shot.position[0], shot.position[1],
        shot.position[2], shot.speed, fired ? "fired" : "not fired");
  }
}

}  // namespace kcds::game_client
