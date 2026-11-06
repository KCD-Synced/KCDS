#include "npc_use.hpp"

#include <atomic>
#include <cstring>

#include "../physics/line_of_sight.hpp"
#include "../props/prop_entity.hpp"

namespace kcds::runtime::npcs::use {

namespace {

// The game's own use action (dialogue/dialogue_list.cpp kPickAction) and its press mode.
constexpr const char* kUseAction = "use";
constexpr unsigned kPressed = 1;

Natives natives;
std::atomic<bool> pressed{false};

}  // namespace

void install(const Natives& given) {
  natives = given;
}

void on_action(const char* action, unsigned mode) {
  if (action && mode == kPressed && std::strcmp(action, kUseAction) == 0) {
    pressed.store(true);
  }
}

void frame(bool in_world) {
  if (!pressed.exchange(false) || !in_world) {
    return;
  }
  float camera[12];
  if (!natives.view_camera || !natives.view_camera(camera) || !natives.npc_of_engine ||
      !natives.send) {
    return;
  }
  const float from[3] = {camera[3], camera[7], camera[11]};
  const float to[3] = {from[0] + camera[1] * kUseReach, from[1] + camera[5] * kUseReach,
                       from[2] + camera[9] * kUseReach};
  void* own = natives.own_physics ? natives.own_physics() : nullptr;
  physics::RayHit hit;
  const physics::TraceResult result =
      physics::trace(from, to, physics::kHitAnything, own ? &own : nullptr, own ? 1 : 0, hit);
  if (result != physics::TraceResult::Hit || hit.terrain) {
    return;
  }
  void* entity = physics::entity_of(hit.collider);
  const std::uint32_t npc = entity ? natives.npc_of_engine(props::entity::id_of(entity)) : 0;
  if (!npc) {
    return;
  }
  const bool sent = natives.send(npc);
  if (natives.log) {
    natives.log("NPC use npc=%u at %.2f m sent=%u", npc, hit.distance, unsigned(sent));
  }
}

}  // namespace kcds::runtime::npcs::use
