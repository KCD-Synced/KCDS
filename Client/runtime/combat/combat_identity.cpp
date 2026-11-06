// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "combat_identity.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/remote.hpp"
#include "player_damage.hpp"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// native IActionListener::OnAction uses RCX self, RDX ActionId&, R8 mode,
// XMM3 value. Eligibility receives two real C_Soul pointers, not ActorData. (CombatInput and
// CombatDamage: core/client_internal.hpp; CombatEligibility: combat_identity.hpp.)
CombatInput original_combat_input;
CombatEligibility original_combat_eligibility;
CombatDamage original_combat_damage;
std::atomic<void*> local_combat_actor{nullptr};
std::mutex combat_identity_mutex;
std::unordered_map<void*, core::EntityId> remote_combat_souls;
std::unordered_map<void*, void*> remote_combat_movements;
// A replica's own game keeps lowering a bleeding replica's health every frame, and every write is
// ignored (its owner's health is kept): one log line per replica and second says so, with how many
// writes were ignored since the previous line (IgnoredHealthWrites, combat/player_damage.hpp).
// Guarded by combat_identity_mutex.
std::unordered_map<core::EntityId, IgnoredHealthWrites> ignored_health_writes;

// Whether a soul is a remote player's replica's (any thread).
bool is_replica_soul(void* soul) {
  std::lock_guard<std::mutex> lock(combat_identity_mutex);
  return remote_combat_souls.count(soul) != 0;
}

thread_local void* remote_manual_combat_soul = nullptr;
unsigned native_action_type(void* movement);
bool native_attack_active(void* movement);
thread_local void* replicated_attack_actor = nullptr;
// The local fighter while a replica's pair starts against it as on the owner's screen, where the
// local fighter is a replica: no channel (hooked_actor_has_channel).
thread_local void* replicated_opponent_actor = nullptr;

std::atomic<core::EntityId> local_network_id{core::kInvalidEntity};
std::atomic<core::EntityId> local_requested_target{core::kInvalidEntity};
thread_local unsigned manual_combat_scope = 0;
thread_local bool replaying_combat_input = false;
// While a replica replays an attack press its owner's game made nothing of, just before the release
// that did make an attack (dropped_attack_presses): the owner's pair selection belongs to
// that release (hooked_make_attack).
thread_local bool replaying_refused_press = false;
// Whether the local player's own attack factory made an attack during the input the game is
// handling now: a press or release that made none and asked for no riposte goes to the
// replicas marked refused (a release the game holds for a later update is not refused,
// attack_release_held).
thread_local bool local_attack_made = false;
// The players whose next attack press or release their own game made nothing of (AttackRefused),
// and the attack press of each that was left out (its input action; null after a left-out
// release). Engine thread.
std::unordered_set<core::EntityId> refused_attack_inputs;
std::unordered_map<core::EntityId, const void*> dropped_attack_presses;
// The players whose next attack press or release asked their own game for the riposte (Riposte).
// Engine thread.
std::unordered_set<core::EntityId> riposte_attack_inputs;
// Whom each replica's owner has its combat lock on (key: the replica's Soul): that player's network
// id (the local player's or another remote's), 0 for nobody. Protected by combat_identity_mutex.
std::unordered_map<void*, core::EntityId> remote_native_targets;
// The Soul of the player with network id `id` here: the local player's or its replica's. The
// caller holds combat_identity_mutex.
void* combat_soul_of(core::EntityId id) {
  if (!id) {
    return nullptr;
  }
  if (id == local_network_id.load()) {
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    return actor ? *reinterpret_cast<void**>(actor + 1640) : nullptr;
  }
  for (const auto& [soul, owner] : remote_combat_souls) {
    if (owner == id) {
      return soul;
    }
  }
  return nullptr;
}
// The IEntity of the player whose Soul is `soul` here. The caller holds combat_identity_mutex.
void* combat_entity_of(void* soul) {
  auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  if (actor && soul == *reinterpret_cast<void**>(actor + 1640)) {
    return *reinterpret_cast<void**>(actor + 56);
  }
  auto movement = remote_combat_movements.find(soul);
  if (movement == remote_combat_movements.end() || !movement->second) {
    return nullptr;
  }
  auto remote =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement->second) + 504);
  return remote ? *reinterpret_cast<void**>(remote + 56) : nullptr;
}
SetCombatTarget original_set_combat_target;
// After a relayed hit on a locked victim, its own hit feedback must not clear the lock
// until keep_lock_until, clearing the local player's target re-selects the
// attacker (keep_lock_entity, its IEntity) instead (kKeepLockMs, kHardLockFlag:
// combat_identity.hpp).
std::atomic<bool> keep_hard_lock{false};
std::atomic<void*> keep_lock_entity{nullptr};
std::atomic<ULONGLONG> keep_lock_until{0};
std::atomic<void*> lost_lock_entity{nullptr};
std::atomic<ULONGLONG> lost_lock_at{0};
std::atomic<std::uint8_t> lost_lock_hard{0};
std::intptr_t __fastcall hooked_set_combat_target(void* movement, void* entity) {
  auto actor = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 504);
  auto soul = actor ? *reinterpret_cast<void**>(actor + 1640) : nullptr;
  bool replica = false;
  void* selected = nullptr;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    if (auto it = remote_native_targets.find(soul); it != remote_native_targets.end()) {
      replica = true;
      selected = combat_entity_of(combat_soul_of(it->second));
    }
  }
  // passes the selected actor's IEntity, not Movement or Soul.
  // Replicas use their owner's selection, never the other client's camera: the local player, or
  // on a watcher's screen the replica of the other player its owner fights.
  if (replica) {
    entity = selected;
  }
  auto core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528);
  auto before = core ? *reinterpret_cast<void**>(core + 3320) : nullptr;
  if (!replica && !entity && before && actor == local_combat_actor.load() &&
      GetTickCount64() < keep_lock_until.load() && keep_lock_entity.load()) {
    // Skip the clear itself: re-selecting the target made the lock marker blink.
    log("COMBAT TARGET clear skipped after a relayed hit actor=%p", actor);
    return 0;
  }
  if (!replica && !entity && before && actor == local_combat_actor.load()) {
    auto controller =
        *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 1104);
    lost_lock_hard.store(controller ? controller[kHardLockFlag] : 0);
    lost_lock_entity.store(before);
    lost_lock_at.store(GetTickCount64());
  }
  auto result = original_set_combat_target(movement, entity);
  auto after = core ? *reinterpret_cast<void**>(core + 3320) : nullptr;
  if (before != after)
    log("COMBAT TARGET native actor=%p replica=%u target=%p->%p caller=%p", actor,
        unsigned(replica), before, after, _ReturnAddress());
  return result;
}
// The local swing that runs now does not land on that opponent (defined with the contacts).
void retire_local_swing(core::EntityId opponent);
// Player.stopFight: when the local player's combat target is that opponent's
// replica, the lock goes (the selection through the game's own setter, and the hard-lock byte).
// A swing the local player started before the cancel does not land on the opponent, locked on or
// not: its contact is not relayed to the opponent's game.
void cancel_combat_with(void* local_actor, std::uint32_t opponent) {
  retire_local_swing(opponent);
  auto movement =
      local_actor ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(local_actor) + 632)
                  : nullptr;
  auto core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
  auto selected = core ? *reinterpret_cast<std::uint8_t**>(core + 3320) : nullptr;
  auto selected_actor = selected ? *reinterpret_cast<std::uint8_t**>(selected + 504) : nullptr;
  auto soul = selected_actor ? *reinterpret_cast<void**>(selected_actor + 1640) : nullptr;
  bool locked_on = false;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    auto it = soul ? remote_combat_souls.find(soul) : remote_combat_souls.end();
    locked_on = it != remote_combat_souls.end() && it->second == opponent;
  }
  log("COMBAT cancel with %u: %s", opponent, locked_on ? "lock dropped" : "not locked on");
  if (!locked_on)
    return;
  keep_lock_until.store(0);
  keep_lock_entity.store(nullptr);
  hooked_set_combat_target(movement, nullptr);
  if (auto controller = *reinterpret_cast<std::uint8_t**>(movement + 1104))
    controller[kHardLockFlag] = 0;
}
integration::CombatAction action_kind(const char* name) {
  using A = integration::CombatAction;
  if (!name)
    return A{};
  static const std::pair<const char*, A> names[] = {{"block", A::Block},
                                                    {"prepare_to_attack", A::Prepare},
                                                    {"attack_primary_mouse", A::AttackMouse},
                                                    {"attack_primary", A::Attack},
                                                    {"attack_abort", A::Abort},
                                                    {"opponent_lock", A::Lock},
                                                    {"combat_unlock_mouse", A::Unlock},
                                                    {"combat_zone_mouse_x", A::ZoneX},
                                                    {"combat_zone_mouse_y", A::ZoneY},
                                                    {"moveleft", A::MoveLeft},
                                                    {"moveright", A::MoveRight},
                                                    {"moveforward", A::MoveForward},
                                                    {"moveback", A::MoveBack},
                                                    {"attack_special", A::Special}};
  for (const auto& n : names)
    if (std::strcmp(name, n.first) == 0)
      return n.second;
  return A{};
}
// How long after the running attack of another player started the local player pressed, in ms;
// -1 when none runs (defined with the replicas).
std::int64_t defence_timing_ms();
void relay_combat_action(const integration::CombatActionData& data) {
  if (!local_network_id.load())
    return;
  if (data.action == integration::CombatAction::Block && data.mode == 1) {
    const std::int64_t timing = defence_timing_ms();
    if (timing >= 0) {
      relay_combat_action({integration::CombatAction::DefenceTiming, 1, float(timing)});
    }
  }
  // These original inputs repeat while held. Relay changes reliably rather
  // than filling combat's ordered channel with identical per-frame presses.
  if (data.action >= integration::CombatAction::MoveLeft &&
      data.action <= integration::CombatAction::MoveBack) {
    static std::uint32_t revision = 0;
    static std::unordered_map<core::EntityId, std::array<integration::CombatActionData, 4>>
        previous;
    if (revision != requested_revision) {
      previous.clear();
      revision = requested_revision;
    }
    const auto index = unsigned(data.action) - unsigned(integration::CombatAction::MoveLeft);
    std::vector<core::EntityId> recipients;
    {
      std::lock_guard<std::mutex> lock(combat_identity_mutex);
      for (const auto& entry : remote_combat_souls)
        recipients.push_back(entry.second);
    }
    for (auto id : recipients) {
      auto& last = previous[id][index];
      if (last.action == data.action && last.mode == data.mode && last.value == data.value)
        continue;
      std::lock_guard<std::mutex> lock(network_mutex);
      if (client->peer().send_combat_action(local_network_id.load(), id, data))
        last = data;
    }
    for (auto it = previous.begin(); it != previous.end();)
      if (std::find(recipients.begin(), recipients.end(), it->first) == recipients.end())
        it = previous.erase(it);
      else
        ++it;
    return;
  }

  std::vector<core::EntityId> recipients;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    for (const auto& entry : remote_combat_souls)
      recipients.push_back(entry.second);
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  for (auto id : recipients)
    if (client->peer().send_combat_action(local_network_id.load(), id, data))
      if (data.action != integration::CombatAction::ZoneX &&
          data.action != integration::CombatAction::ZoneY)
        log("COMBAT ACTION queue source=%u target=%u action=%u mode=%u", local_network_id.load(),
            id, unsigned(data.action), unsigned(data.mode));
}
void relay_native_target(void* actor) {
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + 632);
  auto core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
  auto selected = core ? *reinterpret_cast<std::uint8_t**>(core + 3320) : nullptr;
  auto selected_actor = selected ? *reinterpret_cast<std::uint8_t**>(selected + 504) : nullptr;
  auto soul = selected_actor ? *reinterpret_cast<void**>(selected_actor + 1640) : nullptr;
  auto controller = movement ? *reinterpret_cast<std::uint8_t**>(movement + 1104) : nullptr;
  const bool hard = controller && controller[192];
  core::EntityId target = 0;
  std::vector<core::EntityId> recipients;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    for (const auto& entry : remote_combat_souls) {
      recipients.push_back(entry.second);
      if (entry.first == soul)
        target = entry.second;
    }
  }
  if (hard && target)
    local_requested_target.store(target);
  if (!hard)
    local_requested_target.store(0);
  // What each recipient last got: the selected player's id (0 for nobody).
  static std::unordered_map<core::EntityId, core::EntityId> last;
  for (auto id : recipients) {
    const bool locked = id == target;
    if (last.count(id) && last.at(id) == target)
      continue;
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!client->peer().send_combat_action(local_network_id.load(), id,
                                           {integration::CombatAction::Target,
                                            std::uint8_t(locked ? 1 : 2), locked ? 1.f : 0.f})) {
      continue;
    }
    // A client the lock is not on learns whom it is on, for its replica of this player.
    if (!locked && !client->peer().send_combat_action(
                       local_network_id.load(), id,
                       {integration::CombatAction::TargetOther, 1, float(target)})) {
      continue;
    }
    last[id] = target;
    log("COMBAT TARGET queue source=%u recipient=%u selected=%u hard=%u", local_network_id.load(),
        id, target, unsigned(hard));
  }
  for (auto it = last.begin(); it != last.end();)
    if (std::find(recipients.begin(), recipients.end(), it->first) == recipients.end())
      it = last.erase(it);
    else
      ++it;
}
}  // namespace kcds::game_client
