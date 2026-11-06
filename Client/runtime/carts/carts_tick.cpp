#include "carts_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../physics/line_of_sight.hpp"
#include "../props/prop_entity.hpp"
#include "placed_carts.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace kcds::game_client {

// The local player's body entity, or null.
void* local_body_entity() {
  auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  return actor ? *reinterpret_cast<void**>(actor + 56) : nullptr;
}

// The carts' view of the local player: where its body stands, and putting it there
// as the script SetWorldPos does (SetWorldTM, entity slot 30, reason 0x400000).
bool cart_local_pose(float position[3], float turn[4]) {
  void* entity = local_body_entity();
  const float* tm = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
  if (!tm) {
    return false;
  }
  position[0] = tm[3];
  position[1] = tm[7];
  position[2] = tm[11];
  const core::Quat q = rotation(tm);
  turn[0] = q.x;
  turn[1] = q.y;
  turn[2] = q.z;
  turn[3] = q.w;
  return true;
}

void cart_place_local(const float position[3], const float turn[4]) {
  void* entity = local_body_entity();
  const float* tm = entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
  if (!tm) {
    return;
  }
  const core::Quat q = turn ? core::Quat{turn[0], turn[1], turn[2], turn[3]} : rotation(tm);
  set_transform(entity, {position[0], position[1], position[2]}, q, kScriptSetWorldPosReason);
}

// The terrain's height under a world point, from `above` down (the carts stand on it).
bool cart_ground_height(float x, float y, float above, float& z) {
  const float from[3] = {x, y, above};
  const float to[3] = {x, y, above - 30.0f};
  kcds::runtime::physics::RayHit hit;
  const auto result = kcds::runtime::physics::trace(
      from, to, kcds::runtime::physics::kHitTerrainOnly, nullptr, 0, hit);
  if (result != kcds::runtime::physics::TraceResult::Hit) {
    return false;
  }
  z = hit.position[2];
  return true;
}

// Whether something solid other than the terrain lies between two world points (the carts stop
// for it).
// Whether an entity is one of the parts of cart `own` (kcds_cart_<own> or kcds_cart_<own>_...).
static bool part_of(const std::string& name, std::uint32_t own) {
  const std::string prefix = "kcds_cart_" + std::to_string(own);
  if (name.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  return name.size() == prefix.size() || name[prefix.size()] == '_';
}

bool cart_blocked(const float from[3], const float to[3], std::uint32_t own) {
  namespace physics = kcds::runtime::physics;
  // Not the terrain: a wagon goes up and down the slopes its horses walk. The cart's own parts
  // are looked through: the ray goes on just past them.
  const int solid = physics::kHitCover & ~physics::kHitTerrainOnly;
  constexpr int kPastOwnParts = 4;
  constexpr float kPastStep = 0.02f;
  float start[3] = {from[0], from[1], from[2]};
  physics::RayHit hit;
  std::string name;
  bool found = false;
  for (int pass = 0; pass < kPastOwnParts; ++pass) {
    if (physics::trace(start, to, solid, nullptr, 0, hit) != physics::TraceResult::Hit) {
      return false;
    }
    void* entity = physics::entity_of(hit.collider);
    name = entity ? kcds::runtime::props::entity::name_of(entity, 64) : std::string();
    if (!part_of(name, own)) {
      found = true;
      break;
    }
    const float dx = to[0] - start[0];
    const float dy = to[1] - start[1];
    const float dz = to[2] - start[2];
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length < kPastStep) {
      return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
      start[axis] = hit.position[axis] + (to[axis] - start[axis]) / length * kPastStep;
    }
  }
  if (!found) {
    return false;
  }
  // What stopped a cart, at most once a second (the log tells a wall from a bush).
  static double logged_at = 0.0;
  const double now = local::clock_seconds();
  if (now - logged_at > 1.0) {
    logged_at = now;
    log("CART blocked by %s at %.2f %.2f %.2f", name.empty() ? "a static object" : name.c_str(),
        hit.position[0], hit.position[1], hit.position[2]);
  }
  return true;
}

bool cart_report(const kcd2s::integration::CartReport& report) {
  std::lock_guard<std::mutex> lock(network_mutex);
  return client->peer().send_cart_report(report);
}

// The carts the server placed, in the order it sent them, then this frame's driving.
void tick_carts() {
  std::vector<kcd2s::integration::CartOrder> orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    orders = client->peer().take_cart_orders();
  }
  for (const auto& order : orders) {
    kcds::runtime::carts::placed::apply(order);
  }
  kcds::runtime::carts::placed::frame(local::clock_seconds());
}
}  // namespace kcds::game_client
