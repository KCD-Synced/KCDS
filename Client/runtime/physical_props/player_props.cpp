// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "player_props.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <unordered_map>

#include "../core/log.hpp"
#include "../pose_animation/hand_grip.hpp"
#include "../pose_animation/player_animations.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/physical_prop.hpp"
#include "rope_prop.hpp"
#include "whip_prop.hpp"

namespace kcds::runtime::physical_props::players {
namespace {

namespace integration = kcd2s::integration;

// CActor: its entity at + 56 (core/frame.cpp reads the local one the same way).
constexpr std::size_t kActorEntity = 56;

struct Held {
  std::string field;
  void* actor = nullptr;
  // The body whose hand closes round the prop (pose_animation/hand_grip.hpp).
  void* body = nullptr;
  // The prop the field names, with its hand, grip and physics.
  std::optional<integration::PhysicalProp> prop;
  std::unique_ptr<RopeProp> rope;
  std::unique_ptr<WhipProp> whip;
};

// The prop's physics as a rope takes them (rope_prop.hpp RopePhysics).
RopePhysics physics_of(const integration::PhysicalProp& prop) {
  RopePhysics physics;
  physics.gravity = prop.gravity;
  physics.damping = prop.damping;
  physics.mass = prop.mass;
  physics.air_resistance = prop.air;
  physics.bend_limit = prop.bend;
  physics.stiffness = prop.stiffness;
  physics.collide = prop.collide;
  physics.shape = prop.shape;
  physics.curl = prop.curl;
  physics.sway = prop.sway;
  return physics;
}

// Kept for the game's life (its props must not be taken down at the process's end).
std::unordered_map<std::uint32_t, Held>& held() {
  static auto* map = new std::unordered_map<std::uint32_t, Held>();
  return *map;
}

void take_down(Held& one) {
  one.rope.reset();
  one.whip.reset();
  if (one.body) {
    pose_animation::hand_grip::forget(one.body);
    one.body = nullptr;
  }
}

void* entity_of(void* actor) {
  return actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + kActorEntity)
               : nullptr;
}

// Builds the prop the field names on the body (none for ""); false when it could not yet.
bool build(std::uint32_t player, Held& one, const float hand[3]) {
  take_down(one);
  const auto prop = integration::physical_prop_of(one.field);
  one.prop = prop;
  if (!prop) {
    return true;
  }
  if (prop->kind == integration::PhysicalPropKind::Whip) {
    one.whip = std::make_unique<WhipProp>();
    const std::string name = "kcds_physprop_" + std::to_string(player);
    if (!one.whip->make(name, prop->segments, prop->length, hand)) {
      one.whip.reset();
      return false;
    }
  } else {
    // A chain is a rope drawn with the game's chain links.
    const bool chain = prop->kind == integration::PhysicalPropKind::Chain;
    one.rope = std::make_unique<RopeProp>();
    if (!one.rope->make(prop->segments, prop->length, hand, chain ? kChainMaterial : nullptr)) {
      one.rope.reset();
      return false;
    }
  }
  game_client::log("PHYSICAL PROP player %u: %s", player, one.field.c_str());
  return true;
}

}  // namespace

void show(std::uint32_t player, void* actor, const std::string& field) {
  Held& one = held()[player];
  void* entity = entity_of(actor);
  if (!entity) {
    if (one.rope || one.whip) {
      take_down(one);
    }
    one.actor = nullptr;
    return;
  }
  // Its settings now: the field's, with the player's animation's prop keys on top.
  const auto field_prop = field == one.field ? one.prop : integration::physical_prop_of(field);
  const integration::PhysicalProp now = integration::physical_prop_with(
      field_prop.value_or(integration::PhysicalProp{}), pose_animation::players::prop_now(player));
  // Held in the palm: the game's own grip joint there (where it puts weapons), not the wrist.
  const char* joint = now.left_hand ? "LeftHandSlot" : "RightHandSlot";
  float hand[3];
  float turn[4];
  if (!props::entity::joint_pose(entity, joint, hand, turn)) {
    return;
  }
  const bool built = one.rope || one.whip;
  if (field != one.field || actor != one.actor || (!built && !field.empty())) {
    one.field = field;
    one.actor = actor;
    if (!build(player, one, hand)) {
      return;
    }
  }
  // The hand that holds it closes round it (the other one stays as it is animated).
  if (one.rope || one.whip) {
    if (one.body && one.body != entity) {
      pose_animation::hand_grip::forget(one.body);
    }
    one.body = entity;
    pose_animation::hand_grip::hold(entity, true, now.left_hand);
  }
  if (one.rope) {
    one.rope->set_own_physics(physics_of(now));
    one.rope->hold(hand);
  } else if (one.whip) {
    // The left hand's joint is the right's mirrored: the same grip turns the other way round
    // its axes (as the studio's editor sends it), so the handle points out of that fist too.
    const float mirrored = now.left_hand ? -1.0f : 1.0f;
    one.whip->set_grip(mirrored * now.pitch, mirrored * now.twist);
    one.whip->set_physics(physics_of(now));
    // The lash stays above the ground the player stands on (the body's feet).
    float feet[3];
    float facing[4];
    props::entity::world_pose(entity, feet, facing);
    one.whip->set_ground(feet[2]);
    // The player's own body in the lash's way when it does not ignore characters (collide 1).
    one.whip->set_holder(entity, now.left_hand);
    // The lash's shape from the player's animation keys (Lash to keys), else physics; in the
    // body's frame when the animation's lash track is kept so.
    const bool in_body = pose_animation::players::lash_in_body(player);
    one.whip->set_lash_shape(pose_animation::players::lash_now(player), in_body ? facing : nullptr);
    one.whip->place(hand, turn);
  }
}

void keep_only(const std::vector<std::uint32_t>& shown) {
  auto& map = held();
  for (auto it = map.begin(); it != map.end();) {
    if (std::find(shown.begin(), shown.end(), it->first) == shown.end()) {
      take_down(it->second);
      it = map.erase(it);
    } else {
      ++it;
    }
  }
}

void reset() {
  auto& map = held();
  for (auto& [player, one] : map) {
    take_down(one);
  }
  map.clear();
}

}  // namespace kcds::runtime::physical_props::players
