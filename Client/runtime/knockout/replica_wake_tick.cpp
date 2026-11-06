// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "replica_wake_tick.hpp"

#include "../carry/body_carry.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/motion_capture.hpp"
#include "../riding/trample.hpp"
#include "knockout.hpp"
#include "replica_wake.hpp"

#include <mutex>
#include <unordered_set>
#include <optional>

namespace kcds::game_client {

// Native scope 7 belongs to the fallen action and scope 9 to its authored GetUp successor.
// These observations do not clear restrictions or infer completion from a standing mesh.
void observe_replica_wake_restriction(core::EntityId player, void* gate) {
  if (GetCurrentThreadId() != engine_update_thread.load()) {
    return;
  }
  auto found = remotes.find(player);
  if (found == remotes.end() || !found->second.wake_scope) {
    return;
  }
  auto& remote = found->second;
  const auto scope = *remote.wake_scope;
  auto actor = static_cast<std::uint8_t*>(remote.actor);
  auto soul = actor ? *reinterpret_cast<void**>(actor + 1640) : nullptr;
  auto model = actor ? *reinterpret_cast<std::uint8_t**>(actor + 2448) : nullptr;
  if (!model || model + 920 != gate || scope.revision != requested_revision ||
      scope.actor != reinterpret_cast<std::uintptr_t>(actor) ||
      scope.soul != reinterpret_cast<std::uintptr_t>(soul) ||
      scope.model != reinterpret_cast<std::uintptr_t>(model)) {
    return;
  }
  const auto body = kcds::runtime::knockout::observe_body(soul, soul, native_soul_is_unconscious);
  if (body == kcds::runtime::knockout::BodyState::Unavailable) {
    return;
  }
  remote.wake.observe_native(scope, {body == kcds::runtime::knockout::BodyState::KnockedOut,
                                     model[951] != 0, model[953] != 0});
}

void observe_replica_wake_owner(core::EntityId player, Remote& remote, bool down) {
  auto actor = static_cast<std::uint8_t*>(remote.actor);
  auto soul = actor ? *reinterpret_cast<void**>(actor + 1640) : nullptr;
  auto model = actor ? *reinterpret_cast<void**>(actor + 2448) : nullptr;
  std::optional<std::uint8_t> epoch;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    epoch = client->peer().combat_epoch(player);
  }
  if (!actor || !soul || !model || !epoch) {
    remote.wake.bind({});
    remote.wake_scope.reset();
    return;
  }
  const kcds::runtime::knockout::ReplicaWakeScope scope{
      requested_revision, *epoch, reinterpret_cast<std::uintptr_t>(actor),
      reinterpret_cast<std::uintptr_t>(soul), reinterpret_cast<std::uintptr_t>(model)};
  remote.wake.continue_body(scope);
  remote.wake_scope = scope;
  remote.wake.observe_owner(scope, down);
}

// The game lays an unconscious actor down through its ragdoll action (actor action kind 8, pushed
// from the actor state's update and a fall's end); that
// action then owns the body: its own ragdoll, the fallen restriction and the authored GetUp. A
// replica held down for its owner (keep_replica_down) lies by its owner's ragdoll pose instead
// (tick_owner_ragdoll) and gets up with its owner's clips: the game's own ragdoll fought the pose
// (the body flipped over on a watcher's screen) and its GetUp was the short one without the
// knock-out, while the attacker's game, busy with the blow's reaction, pushed none and showed the
// owner's stand-up. The action is the actor data's (actor + 640) expansion of type 1, as
// looks it up (vfunc 14). A replica held down for its owner's exhaustion faint is the
// exception: a faint has no blow, so the owner's body may never become a ragdoll to
// send its pose, and the replica stood; its own game lays it down through this action and gets it
// up by the native wake (tick_replica_wake).
PushRagdollAction original_push_ragdoll_action = nullptr;
// The knocked-out replicas this game lays down through its own ragdoll action (see
// replica_laid_by_game): the action was pushed while the replica was down.
std::unordered_set<core::EntityId> laid_by_game_action;
constexpr std::size_t kActorData = 640;
constexpr std::size_t kActorDataExpansion = 14;
constexpr int kActorStateExpansion = 1;

char __fastcall hooked_push_ragdoll_action(void* expansion, void* unused, void* kind) {
  if (expansion && GetCurrentThreadId() == engine_update_thread.load()) {
    for (const auto& [player, remote] : remotes) {
      if (!remote.actor || !kcds::runtime::knockout::replica_down(player)) {
        continue;
      }
      void* data = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(remote.actor) + kActorData);
      void* state = data ? slot<void*(__fastcall*)(void*, int)>(data, kActorDataExpansion)(
                               data, kActorStateExpansion)
                         : nullptr;
      if (state == expansion) {
        if (kcds::runtime::knockout::replica_fainted(player)) {
          log("KNOCKOUT replica of player=%u laid down by the game's own ragdoll action (its "
              "owner fainted)",
              player);
          break;
        }
        // A carry putting the body down lets it drop here (the corpse carry's put-down): the
        // replica takes its presentation ragdoll instead, laid out by its owner's pose as the
        // owner's own body drops. Left as it was, the body stood up in every other game while its
        // owner still lay knocked out.
        const bool put_down =
            kcds::runtime::carry::body_held(player, remote.engine_id, local_combat_actor.load());
        // A knock-out: this game lays the replica down itself, as it lays down a knocked-out NPC,
        // and its owner's pose is no longer put on it. Following its owner's ragdoll pose update
        // by update, the body fell in steps and stiffly.
        // Where the game pushes no action (the attacker's game, busy with the blow's
        // reaction) the replica still lies by its owner's pose.
        if (!put_down) {
          laid_by_game_action.insert(player);
          log("KNOCKOUT replica of player=%u laid down by the game's own ragdoll action (knocked "
              "out)",
              player);
          break;
        }
        bool dropped = false;
        if (put_down) {
          dropped = set_physics_profile(remote.actor, PhysicsProfile::Sleep);
          owner_ragdolled.insert(player);
          applied_ragdoll_poses.erase(player);
        }
        log("KNOCKOUT replica of player=%u kept out of the game's own ragdoll action put_down=%u "
            "dropped=%u",
            player, unsigned(put_down), unsigned(dropped));
        return 0;
      }
    }
  }
  return original_push_ragdoll_action(expansion, unused, kind);
}

bool replica_laid_by_game(core::EntityId player) {
  if (kcds::runtime::knockout::replica_fainted(player)) {
    return true;
  }
  if (!kcds::runtime::knockout::replica_down(player)) {
    laid_by_game_action.erase(player);
    return false;
  }
  return laid_by_game_action.count(player) != 0;
}

bool tick_replica_wake(core::EntityId player, Remote& remote, void* entity) {
  if (!remote.wake_scope) {
    return false;
  }
  const auto scope = *remote.wake_scope;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    const auto epoch = client->peer().combat_epoch(player);
    if (!epoch || *epoch != scope.epoch) {
      return true;
    }
  }
  auto actor = static_cast<std::uint8_t*>(remote.actor);
  if (!actor || !entity || *reinterpret_cast<void**>(actor + 56) != entity) {
    return true;
  }
  auto soul = *reinterpret_cast<void**>(actor + 1640);
  auto model = *reinterpret_cast<std::uint8_t**>(actor + 2448);
  if (!model || scope.revision != requested_revision ||
      scope.actor != reinterpret_cast<std::uintptr_t>(actor) ||
      scope.soul != reinterpret_cast<std::uintptr_t>(soul) ||
      scope.model != reinterpret_cast<std::uintptr_t>(model)) {
    return true;
  }
  const bool native_getup = model[953] != 0;
  const auto body = kcds::runtime::knockout::observe_body(soul, soul, native_soul_is_unconscious);
  if (body != kcds::runtime::knockout::BodyState::Unavailable) {
    remote.wake.observe_native(scope, {body == kcds::runtime::knockout::BodyState::KnockedOut,
                                       model[951] != 0, native_getup});
  }
  if (!remote.wake.owns_body() && !native_getup) {
    return false;
  }
  // Only an actual native fallen/GetUp lifecycle releases the presentation ragdoll.
  // A replica put into Sleep by owner-pose replication has no fallen action to retire;
  // its existing owner-ragdoll lifecycle must remain able to restore its own profile.
  if (model[951] || native_getup || remote.wake.requested()) {
    owner_ragdolled.erase(player);
    applied_ragdoll_poses.erase(player);
  }
  release_remote_human_overlay(remote);
  {
    std::lock_guard<std::shared_mutex> lock(motion_mutex);
    remote_inputs.erase(remote.skeleton);
  }
  if (body == kcds::runtime::knockout::BodyState::Unavailable) {
    return true;
  }

  // Original wake retires the fallen action and queues GetUp. Neither owner clips nor the
  // legacy direct Alive profile may cut that native action's restriction/attachment cleanup.
  auto state = *reinterpret_cast<std::uint8_t**>(actor + 400);
  auto data = *reinterpret_cast<void**>(actor + 640);
  const bool ready = native_request_replica_wake && state && data &&
                     *reinterpret_cast<void**>(state + 136) == actor && !state[170] &&
                     body == kcds::runtime::knockout::BodyState::Awake &&
                     slot<bool(__fastcall*)(void*)>(model, 50)(model) &&
                     !slot<bool(__fastcall*)(void*, unsigned)>(model, 83)(model, 20);
  if (remote.wake.request(scope, ready)) {
    log("KNOCKOUT replica native wake requested player=%u epoch=%u", player, unsigned(scope.epoch));
    request_replica_native_wake(actor, state);
    // The original can synchronously retire actions and invoke callbacks. Do not reuse
    // cached action/model pointers or treat this void return as a completion result.
  }
  return true;
}

}  // namespace kcds::game_client
