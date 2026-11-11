// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "replica_spawn.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../ground/ground_items.hpp"
#include "../inventory/kettles.hpp"
#include "../npcs/npc_body.hpp"
#include "../npcs/npc_suppression.hpp"
#include "../ranged/landings.hpp"
#include "MinHook.h"
#include "motion_capture.hpp"
#include "remote.hpp"

#include <mutex>

namespace kcds::game_client {

SpawnEntity original_spawn;
thread_local bool spawning_remote = false;
thread_local bool spawning_female_remote = false;

NativeMovementRequest local_motion;
bool local_motion_valid = false;
void* __fastcall hooked_movement_request(void* animated, const NativeMovementRequest* request) {
  NativeMovementRequest override_request;
  {
    std::shared_lock<std::shared_mutex> lock(motion_mutex);
    const auto* remote = remote_inputs.by_animated(animated);
    if (remote && remote->valid && !native_pair_alignment(remote->movement)) {
      // Every request of the frame carries the turn toward its owner's heading: the animated
      // character keeps only the frame's last request (overwrites it), and in the
      // lock stance the replica's own combat movement asks again after the first one. Giving the
      // turn to the first request only left the replica facing away from its opponent.
      override_request = remote->motion;
      // The turn is the one of the last replica pass. A frame the pass left this replica out of
      // (distance, a calm body) repeats the rest of the request but not the turn: given again,
      // the body turned twice as far, swung back the next frame and spun while its owner turned.
      if (remote->turn_pass != 0 && remote->turn_pass != replica_passes.load()) {
        override_request.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
      }
      request = &override_request;
    }
  }
  auto result = original_movement_request(animated, request);
  bool captured_horse = false;
  {
    std::shared_lock<std::shared_mutex> lock(motion_mutex);
    for (const auto& [skeleton, h] : horse_captures) {
      if (h.animated == animated && h.authority) {
        captured_horse = true;
        break;
      }
    }
  }
  if (captured_horse) {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    for (auto& [skeleton, h] : horse_captures)
      if (h.animated == animated && h.authority) {
        h.motion = *request;
        h.valid = true;
        break;
      }
  }
  if (*reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(animated) + 48) ==
          local_engine_id.load() &&
      local_engine_id.load() != 0) {
    {
      std::lock_guard<std::shared_mutex> lock(motion_mutex);
      local_motion = *request;
      local_motion_valid = true;
    }
    static double next = 0;
    const double now = local::clock_seconds();
    if (now > next) {
      next = now + 2;
      log("LOCAL MOTION type=%u velocity=(%.3f %.3f %.3f) jump=%u strafe=%u", request->type,
          request->velocity.x, request->velocity.y, request->velocity.z, unsigned(request->jumping),
          unsigned(request->allow_strafe));
    }
  }
  return result;
}
// PlayerMovement factory writes CombatModule+40. Its Init also sets
// CombatServices+56 -> input controller+16. Both selectors belong to the local player.
void retain_local_input(void* local_actor) {
  auto movement = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 632);
  if (!movement)
    return;
  auto combat = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(get_services()) + 248);
  *reinterpret_cast<void**>(static_cast<std::uint8_t*>(combat) + 40) = movement;
  auto services = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(movement) + 520);
  if (services)
    if (auto input = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + 56))
      *reinterpret_cast<void**>(static_cast<std::uint8_t*>(input) + 16) = movement;
}

void* __fastcall hooked_spawn(void* system, void* params, bool initialize) {
  if (kcds::runtime::npcs::refuse_spawn(params)) {
    return nullptr;
  }
  // An item the game put into the world while it runs exists on this client only.
  if (kcds::runtime::ground::refuses_runtime_item() &&
      kcds::runtime::npcs::spawn_class_is(params, "PickableItem")) {
    log("GROUND runtime item %s refused",
        *reinterpret_cast<const char* const*>(static_cast<const std::uint8_t*>(params) + 56));
    return nullptr;
  }
  if (spawning_remote) {
    // CreateActor's SEntitySpawnParams flags are at +64. In WH C_Actor::Init,
    // flag 0x1000000 keeps a non-client actor active rather than sleeping and
    // destroying a movement controller before its PostInit. Verified in.
    auto flags = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(params) + 64);
    *flags |= 0x1000000;
    log("REMOTE active spawn flags=0x%x", *flags);
    if (spawning_female_remote) {
      // A woman's body: the female skeleton goes into the script's properties
      // between the spawn and the init, as System.SpawnEntity sets properties. CreateActor
      // spawns without the init and runs it itself.
      void* body = original_spawn(system, params, false);
      if (!body) {
        return nullptr;
      }
      const bool female = kcds::runtime::npcs::set_female_model(body);
      log("REMOTE female skeleton %s", female ? "set" : "refused");
      if (!initialize) {
        return body;
      }
      return kcds::runtime::npcs::init_entity(system, body, params) ? body : nullptr;
    }
  }
  void* entity = original_spawn(system, params, initialize);
  // The level's own people, when they live in this world (issue 016): kept by their level GUID.
  kcds::runtime::npcs::level_person_spawned(params, entity);
  // The level's own items: every pickable entity, for the sweeps of items others took.
  if (entity && kcds::runtime::npcs::spawn_class_is(params, "PickableItem")) {
    const auto id = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(entity) + 12);
    kcds::runtime::ground::level_item_spawned(id);
    // A projectile that landed is a pickable too.
    kcds::runtime::ranged::pickable_spawned(id);
  }
  // Cooking pots: each trigger and the fireplace its level name holds (SEntitySpawnParams
  // + 56 is the name).
  if (entity && kcds::runtime::npcs::spawn_class_is(params, "KettleActionTrigger")) {
    kcds::runtime::inventory::kettle_spawned(
        *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(entity) + 12),
        *reinterpret_cast<const char* const*>(static_cast<const std::uint8_t*>(params) + 56));
  }
  return entity;
}
void* local_view_wrapper = nullptr;
void* local_view_body = nullptr;
void* local_stealth_watcher = nullptr;
void* local_stealth_player = nullptr;
void retain_local_view() {
  // selects the global context; reads its +104 body.
  // C_Player construction assigns that context even for channel-0 replicas.
  // Save the real local selection before CreateActor, restore it after factory
  // and removal; never leave it pointing to a destroyed remote body.
  if (local_view_wrapper && local_view_body)
    *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_view_wrapper) + 104) =
        local_view_body;
  // -> C_StealthWatcher v2: +32 is the
  // local AIObject used for location/crime queries. Preserve it independently
  // of NPCManager+104; a remote lifetime must not own this singleton selector.
  if (local_stealth_watcher && local_stealth_player)
    *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_stealth_watcher) + 32) =
        local_stealth_player;
}
void diagnose_view(const char* event) {
  auto gui = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(get_services()) + 352);
  if (gui) {
    auto accessor = static_cast<std::uint8_t*>(gui) + 160;
    auto watcher = slot<void*(__fastcall*)(void*)>(accessor, 1)(accessor);
    auto player =
        watcher ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(watcher) + 32) : nullptr;
    if (!local_stealth_watcher && watcher && player) {
      local_stealth_watcher = watcher;
      local_stealth_player = player;
      log("LOCAL STEALTH watcher=%p AIObject=%p", watcher, player);
    }
    log("STEALTH %s watcher=%p AIObject=%p", event, watcher, player);
  }
  auto view = gui ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(gui) + 168) : nullptr;
  if (!view)
    return;
  auto wrapper = slot<void*(__fastcall*)(void*)>(view, 2)(view);
  auto getter = wrapper ? slot<void*(__fastcall*)(void*)>(wrapper, 0) : nullptr;
  auto active = getter ? getter(wrapper) : nullptr;
  if (!local_view_wrapper && wrapper && active) {
    local_view_wrapper = wrapper;
    local_view_body = active;
    log("LOCAL VIEW context=%p body=%p", wrapper, active);
  }
  auto ida = [](void* x) {
    return x ? 0x180000000ULL + reinterpret_cast<std::uint8_t*>(x) - image : 0ULL;
  };
  log("VIEW %s view=%p wrapper=%p active=%p tableIDA=0x%llx getterIDA=0x%llx selectorIDA=0x%llx",
      event, view, wrapper, active, ida(*reinterpret_cast<void**>(wrapper)),
      ida(reinterpret_cast<void*>(getter)),
      ida(reinterpret_cast<void*>(slot<void*(__fastcall*)(void*)>(view, 2))));
}

void install_spawn_hook(void* entity_system, bool& finished) {
  if (!original_spawn && entity_system) {
    auto target = reinterpret_cast<void*>(slot<SpawnEntity>(entity_system, 12));
    if (MH_CreateHook(target, reinterpret_cast<void*>(hooked_spawn),
                      reinterpret_cast<void**>(&original_spawn)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
      log("Spawn hook installation failed");
      failed = true;
      return;
    }
  }
  finished = true;
}

}  // namespace kcds::game_client
