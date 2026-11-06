// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "native_actions.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/remote.hpp"
#include "combat_identity.hpp"
#include "melee_contacts.hpp"

#include <array>
#include <mutex>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// Diagnostic: the local C_Player feedback component (+3272) was observed null after a remote
// player reconnected. Log it around remote spawn and removal to find the operation that clears it.
void log_local_feedback(const char* event, void* remote_actor) {
  auto local = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto local_feedback = local ? *reinterpret_cast<void**>(local + 3272) : nullptr;
  auto remote_feedback =
      remote_actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(remote_actor) + 3272)
                   : nullptr;
  log("FEEDBACK %s local=%p local_feedback=%p remote=%p remote_feedback=%p", event, local,
      local_feedback, remote_actor, remote_feedback);
}
// MCP: the native action driver has priority slots at
// +56/+64. Slot 0 is the primary action. reads its EActionType
// through v14; names the original enum values below.
unsigned native_action_type(void* movement) {
  if (!movement)
    return 28;
  auto driver = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 512);
  if (!driver)
    return 28;
  auto begin = *reinterpret_cast<void***>(driver + 56);
  auto end = *reinterpret_cast<void***>(driver + 64);
  if (begin == end || !begin || !*begin)
    return 28;
  return slot<unsigned(__fastcall*)(void*)>(*begin, 14)(*begin);
}

char preview_replica_contact(void* source_actor, void* target_actor, void* handler, void* hit,
                             float* power, char* flag, const void* context, unsigned state,
                             void* outcome) {
  auto previous_actor = replicated_attack_actor;
  auto previous_opponent = replicated_opponent_actor;
  replicated_attack_actor = source_actor;
  replicated_opponent_actor = target_actor;
  char result = 0;
  __try {
    result = produce_native_hit(handler, hit, power, flag, context, false, state, outcome);
  } __finally {
    replicated_attack_actor = previous_actor;
    replicated_opponent_actor = previous_opponent;
  }
  return result;
}

void apply_observer_contact(const integration::MeleeEvent& impact, void* source_actor,
                            unsigned source_engine, void* target_actor, unsigned target_engine) {
  if (!source_actor || !target_actor || source_actor == local_combat_actor.load() ||
      target_actor == local_combat_actor.load() || impact.contact_data[304]) {
    return;
  }
  auto source_move =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(source_actor) + 632);
  auto target_move =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(target_actor) + 632);
  const auto scope = replica_contact_scope(impact.source, source_actor, source_move);
  const auto tracked = replica_contacts.find(impact.source);
  if (!scope || tracked == replica_contacts.end() || !target_move ||
      native_action_type(target_move) != kNativeDefenceAction) {
    return;
  }
  auto core = *reinterpret_cast<std::uint8_t**>(source_move + 528);
  auto handler = *reinterpret_cast<void**>(source_move + 904);
  auto driver = *reinterpret_cast<std::uint8_t**>(source_move + 512);
  auto begin = driver ? *reinterpret_cast<void***>(driver + 56) : nullptr;
  auto end = driver ? *reinterpret_cast<void***>(driver + 64) : nullptr;
  auto action = begin && begin != end ? *begin : nullptr;
  const auto kind = native_action_type(source_move);
  if (!core || !handler || !action || (kind != 3 && kind != 4) ||
      slot<unsigned(__fastcall*)(void*)>(core + 48, 1)(core + 48) != 8 ||
      *reinterpret_cast<void**>(core + 3320) != target_move) {
    return;
  }
  unsigned state = 0;
  std::memcpy(&state, impact.contact_data.data() + 453, 4);
  if (state == *native_backoff_attack_type ||
      !tracked->second.consume(*scope, impact.attack_generation,
                               reinterpret_cast<std::uintptr_t>(action), kind)) {
    return;
  }
  alignas(16) integration::NativeReactionData hit{};
  std::copy_n(impact.contact_data.begin(), 160, hit.begin());
  alignas(16) std::array<std::uint8_t, 144> attack{};
  std::copy_n(impact.contact_data.begin() + 160, 144, attack.begin());
  std::memcpy(attack.data() + 8, &source_engine, 4);
  std::memcpy(attack.data() + 36, &target_engine, 4);
  alignas(16) std::array<std::uint8_t, 64> context{};
  std::copy_n(impact.contact_data.begin() + 304, 16, context.begin());
  alignas(16) std::array<std::uint8_t, 128> modifiers{};
  const auto count = impact.contact_data[320];
  std::copy_n(impact.contact_data.begin() + 321, count * 16, modifiers.begin());
  *reinterpret_cast<void**>(context.data() + 16) = target_move;
  *reinterpret_cast<void**>(context.data() + 24) = attack.data();
  *reinterpret_cast<void**>(context.data() + 32) = modifiers.data();
  *reinterpret_cast<void**>(context.data() + 40) = modifiers.data() + count * 16;
  *reinterpret_cast<void**>(context.data() + 48) = modifiers.data() + count * 16;
  float power = 0;
  std::memcpy(&power, impact.contact_data.data() + 449, 4);
  char native_flag = 0;
  std::array<std::uint8_t, 13> outcome{};
  std::copy_n(impact.contact_data.begin() + 457, 13, outcome.begin());
  prepare_native_contact_target(core, target_move);
  const auto produced =
      preview_replica_contact(source_actor, target_actor, handler, hit.data(), &power, &native_flag,
                              context.data(), state, outcome.data());
  finish_native_physical_contact(source_move, core, power, native_flag);
  log("COMBAT observer preview source=%u target=%u generation=%llu produced=%u flag=%u",
      impact.source, impact.target, impact.attack_generation, unsigned(produced),
      unsigned(static_cast<unsigned char>(native_flag)));
}
// MCP searches all 13 priority slots, not just slot zero.
// creates native C_CombatActorActionAttack with type 3 or 4.
// Only an actually active native action extends hit eligibility after release.
bool native_attack_active(void* movement) {
  if (!movement)
    return false;
  auto driver = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 512);
  if (!driver)
    return false;
  auto begin = *reinterpret_cast<void***>(driver + 56);
  auto end = *reinterpret_cast<void***>(driver + 64);
  const auto bytes =
      reinterpret_cast<std::uintptr_t>(end) - reinterpret_cast<std::uintptr_t>(begin);
  if (!begin || bytes % sizeof(void*) || bytes / sizeof(void*) > 13)
    return false;
  for (auto entry = begin; entry != end; ++entry)
    if (*entry) {
      const auto kind = slot<unsigned(__fastcall*)(void*)>(*entry, 14)(*entry);
      if ((kind == 3 || kind == 4) && slot<char(__fastcall*)(void*)>(*entry, 4)(*entry))
        return true;
    }
  return false;
}
// Identify the native neutral attack for the scoped original space-query filter.
bool native_neutral_attack_active(void* movement) {
  if (!movement)
    return false;
  auto core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528);
  if (!core || *reinterpret_cast<void**>(core + 3320))
    return false;
  return native_attack_active(movement);
}
bool paired_action(unsigned kind) {
  return (kind >= 9 && kind <= 11) || (kind >= 18 && kind <= 24);
}
// MCP 913574 -> 90BED0: clinch holding is a Guard (type 0)
// owning GuardSyncMovement (type 15) at +128, not a primary clinch action.
// Its native movement helper aligns the pair. Preserve that alignment only
// while a guard owns this action, or while native SyncAttack/SyncHit are
// linked through source+160 and victim+192 (D4B398). FIFO is unchanged.
bool has_guard_sync_movement(void* movement) {
  if (native_action_type(movement) != 0)
    return false;
  auto driver = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 512);
  auto begin = *reinterpret_cast<void***>(driver + 56);
  auto guard = static_cast<std::uint8_t*>(*begin);
  auto sync = *reinterpret_cast<void**>(guard + 128);
  return sync && slot<unsigned(__fastcall*)(void*)>(sync, 14)(sync) == 15;
}
// The game's own clinch sets each fighter's guard zone from its rows (90BED0 ->)
// and its pairs change it again. Every guard change is signalled at core + 264,
// and the clinch component turns that signal into a clinch transition: sets
// its + 27 while the fighter is in its guard state, and its update starts a
// SyncTransition through, paired with the opponent. While a replica holds a clinch or
// plays a paired action here, its guard is the game's, and the owner's relayed guard is not
// written into it (written a moment after the clinch start, it made both fighters start a
// transition the owner's game never made, and the clinch ended on that screen only).
bool native_clinch_owns_guard(void* movement) {
  return has_guard_sync_movement(movement) || paired_action(native_action_type(movement));
}
// The owner's guard is still its player's choice of the next transition: a mouse zone in the clinch
// changes the guard (->), and that change starts the transition. While a
// replica holds a clinch here, a relayed guard is therefore written once, on receipt, when its
// owner sent it from the clinch state (core + 96) this replica holds here; the replica's clinch
// component then starts the same transition the owner's did, on every screen (written
// never, the side transition played on the owner's screen only). A guard the owner sent before
// its own clinch start, or before the state the replica already holds here, is not written: the
// clinch start or the transition here already set both guards from their rows.
// `owner_model`: the owner's last relayed CombatModel values (kCoreModelProperties order).
int combat_property_value(void* movement, std::size_t property);
bool owner_guard_for_held_clinch(void* movement, const std::array<unsigned, 3>& owner_model) {
  const unsigned owner_state = owner_model[2];
  return has_guard_sync_movement(movement) && owner_state != ~0u &&
         int(owner_state) == combat_property_value(movement, kCoreMode);
}
bool native_pair_alignment(void* movement) {
  if (!movement)
    return false;
  auto own = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto own_move = own ? *reinterpret_cast<std::uint8_t**>(own + 632) : nullptr;
  if (!own_move || movement == own_move)
    return false;
  auto core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528);
  auto own_core = *reinterpret_cast<std::uint8_t**>(own_move + 528);
  if (!core || !own_core || *reinterpret_cast<void**>(core + 3320) != own_move ||
      *reinterpret_cast<void**>(own_core + 3320) != movement)
    return false;
  const auto remote_kind = native_action_type(movement), own_kind = native_action_type(own_move);
  if (remote_kind == 0 && (has_guard_sync_movement(movement) || has_guard_sync_movement(own_move)))
    return true;
  if (!((remote_kind == 22 && own_kind == 23) || (remote_kind == 23 && own_kind == 22)))
    return false;
  auto primary = [](void* m) {
    auto driver = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(m) + 512);
    return static_cast<std::uint8_t*>(**reinterpret_cast<void***>(driver + 56));
  };
  auto source = primary(remote_kind == 22 ? movement : own_move);
  auto victim = primary(remote_kind == 23 ? movement : own_move);
  return *reinterpret_cast<void**>(source + 160) == victim &&
         *reinterpret_cast<void**>(victim + 192) == source;
}
// rejects neutral attack selection when Actor channel is zero.
// A replicated C_Player intentionally has no local input channel. Extend
// only that ownership predicate inside its original attack factory; native
// weapon selection, collision, defense and RPG damage remain original.
ActorHasChannel original_actor_has_channel = nullptr;
bool __fastcall hooked_actor_has_channel(void* actor) {
  const bool native = original_actor_has_channel(actor);
  if (!native && actor && actor == replicated_attack_actor)
    return true;
  if (native && actor && actor == replicated_opponent_actor)
    return false;
  return native;
}
// MCP: observe original input before combat filtering and SyncAttack exit.
// These hooks call originals once and never clear gameplay locks or timers.
void log_native_control_state(const char* stage, void* movement) {
  if (!movement)
    return;
  auto m = static_cast<std::uint8_t*>(movement);
  auto actor = *reinterpret_cast<void**>(m + 504);
  const bool local = actor == local_combat_actor.load();
  const auto id = remote_combat_id(movement);
  if (!local && !id)
    return;
  auto core = *reinterpret_cast<std::uint8_t**>(m + 528);
  auto helper = *reinterpret_cast<std::uint8_t**>(m + 544);
  log("CONTROL %s local=%u source=%u movement=%p action=%u core_bool=%u state=%u clinch=%u "
      "pair_motion=%u pair_token=%u target=%p tick=%llu",
      stage, unsigned(local), id, movement, native_action_type(movement),
      core ? unsigned(slot<unsigned char(__fastcall*)(void*)>(core, 1)(core)) : 0u,
      core ? slot<unsigned(__fastcall*)(void*)>(core + 48, 1)(core + 48) : 0u,
      core ? slot<unsigned(__fastcall*)(void*)>(core + 96, 1)(core + 96) : 0u,
      helper ? unsigned(helper[40]) : 0u, helper ? *reinterpret_cast<unsigned*>(helper + 48) : 0u,
      core ? *reinterpret_cast<void**>(core + 3320) : nullptr, GetTickCount64());
  auto driver = *reinterpret_cast<std::uint8_t**>(m + 512);
  auto begin = driver ? *reinterpret_cast<void***>(driver + 56) : nullptr;
  auto end = driver ? *reinterpret_cast<void***>(driver + 64) : nullptr;
  const auto bytes =
      reinterpret_cast<std::uintptr_t>(end) - reinterpret_cast<std::uintptr_t>(begin);
  if (begin && bytes % sizeof(void*) == 0 && bytes / sizeof(void*) <= 13)
    for (auto entry = begin; entry != end; ++entry)
      if (*entry) {
        auto a = static_cast<std::uint8_t*>(*entry);
        const auto kind = slot<unsigned(__fastcall*)(void*)>(a, 14)(a);
        log("CONTROL action slot stage=%s local=%u source=%u slot=%u kind=%u action=%p running=%u "
            "tick=%llu",
            stage, unsigned(local), id, unsigned(entry - begin), kind, a, unsigned(a[40]),
            GetTickCount64());
        // 90BED0/90C890: Guard owns its paired movement separately.
        if (kind == 0)
          log("CONTROL guard owned stage=%s local=%u source=%u pose=%p auxiliary=%p pair=%p "
              "state=%u substate=%u ready=%u tick=%llu",
              stage, unsigned(local), id, *reinterpret_cast<void**>(a + 112),
              *reinterpret_cast<void**>(a + 120), *reinterpret_cast<void**>(a + 128),
              *reinterpret_cast<unsigned*>(a + 140), *reinterpret_cast<unsigned*>(a + 144),
              unsigned(a[148]), GetTickCount64());
      }
  auto model =
      actor ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + 2448) : nullptr;
  auto controller = *reinterpret_cast<std::uint8_t**>(m + 1104);
  auto filter = controller ? *reinterpret_cast<std::uint8_t**>(controller + 56) : nullptr;
  unsigned restrictions = 0;
  if (model)
    for (unsigned i = 0; i < 21; ++i)
      if (model[944 + i])
        restrictions |= 1u << i;
  log("CONTROL gates stage=%s local=%u source=%u restriction=%u scopes=%x combat_move=%u tick=%llu",
      stage, unsigned(local), id, model ? unsigned(model[965]) : 0u, restrictions,
      filter ? unsigned(filter[22]) : 0u, GetTickCount64());
  // D4C6FC: original paired physical-controller subscriptions.
  log("CONTROL pair subscriptions stage=%s local=%u source=%u own=%u target=%u tick=%llu", stage,
      unsigned(local), id, *reinterpret_cast<unsigned*>(m + 644),
      *reinterpret_cast<unsigned*>(m + 648), GetTickCount64());
}
ULONGLONG last_local_pair_exit = 0;  // Observation window only; never changes gameplay.
SyncAttackExit original_sync_attack_exit = nullptr;
std::intptr_t __fastcall hooked_sync_attack_exit(void* self) {
  auto movement = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + 112);
  log_native_control_state("SyncAttack exit before", movement);
  auto result = original_sync_attack_exit(self);
  log_native_control_state("SyncAttack exit after", movement);
  if (*reinterpret_cast<void**>(static_cast<std::uint8_t*>(movement) + 504) ==
      local_combat_actor.load())
    last_local_pair_exit = GetTickCount64();
  return result;
}
// MCP D4CEC0 preserves frame delta in XMM1. Observe, never poll
// helper v8 twice: those helpers update native gameplay state.
SyncAttackUpdate original_sync_attack_update = nullptr;
unsigned __fastcall hooked_sync_attack_update(void* self, float delta) {
  auto a = static_cast<std::uint8_t*>(self);
  auto movement = *reinterpret_cast<void**>(a + 112);
  auto result = original_sync_attack_update(self, delta);
  auto m = static_cast<std::uint8_t*>(movement);
  auto actor = m ? *reinterpret_cast<std::uint8_t**>(m + 504) : nullptr;
  const bool local = actor && actor == local_combat_actor.load();
  const auto id = remote_combat_id(movement);
  if (local || id) {
    static std::unordered_map<void*, ULONGLONG> next;
    const auto tick = GetTickCount64();
    if (result || tick >= next[movement]) {
      next[movement] = tick + 250;
      auto fragment = *reinterpret_cast<std::uint8_t**>(a + 128);
      auto early = *reinterpret_cast<std::uint8_t**>(a + 152);
      auto partner = *reinterpret_cast<std::uint8_t**>(a + 160);
      // D49EBC: SyncHit fragment is +120; +128/+132 are scalar
      // state/flags, unlike SyncAttack's fragment at +128.
      auto other_fragment = partner ? *reinterpret_cast<std::uint8_t**>(partner + 120) : nullptr;
      auto soul = actor ? *reinterpret_cast<std::uint8_t**>(actor + 1640) : nullptr;
      log("CONTROL SyncAttack update local=%u source=%u self=%p result=%u delta=%.4f finished=%u "
          "fragment=%p fragment_state=%u fragment_flags=%x early_enabled=%u partner=%p "
          "partner_fragment=%p partner_state=%u stamina=%.3f tick=%llu",
          unsigned(local), id, self, result, delta, unsigned(a[120]), fragment,
          fragment ? *reinterpret_cast<unsigned*>(fragment + 40) : 0u,
          fragment ? *reinterpret_cast<unsigned*>(fragment + 44) : 0u,
          early ? unsigned(early[16]) : 0u, partner, other_fragment,
          other_fragment ? *reinterpret_cast<unsigned*>(other_fragment + 40) : 0u,
          soul ? *reinterpret_cast<float*>(soul + 1804) : 0.0f, tick);
      // 4AD9E8: IAction+16 elapsed and +20 native deadline.
      log("CONTROL fragment time local=%u source=%u elapsed=%.4f deadline=%.4f "
          "partner_elapsed=%.4f partner_deadline=%.4f tick=%llu",
          unsigned(local), id, fragment ? *reinterpret_cast<float*>(fragment + 16) : 0.0f,
          fragment ? *reinterpret_cast<float*>(fragment + 20) : 0.0f,
          other_fragment ? *reinterpret_cast<float*>(other_fragment + 16) : 0.0f,
          other_fragment ? *reinterpret_cast<float*>(other_fragment + 20) : 0.0f, tick);
      if (local)
        if (auto skeleton = local_skeleton.load()) {
          const auto count =
              slot<unsigned(__fastcall*)(void*, unsigned)>(skeleton, 15)(skeleton, 0);
          if (count <= 16 && (*reinterpret_cast<void***>(skeleton))[28] ==
                                 reinterpret_cast<void*>(native_animation_progress))
            for (unsigned i = 0; i < count; ++i) {
              auto raw = slot<const std::uint8_t*(__fastcall*)(void*, unsigned, unsigned)>(
                  skeleton, 18)(skeleton, 0, i);
              log("CONTROL root clip entry=%u clip=%d progress=%.4f flags=%x token=%u "
                  "user_token=%u tick=%llu",
                  i, *reinterpret_cast<const int*>(raw + 16),
                  native_animation_progress(skeleton, raw),
                  *reinterpret_cast<const unsigned*>(raw + 76),
                  *reinterpret_cast<const unsigned*>(raw + 48),
                  *reinterpret_cast<const unsigned*>(raw + 84), tick);
            }
        }
      log_native_control_state("SyncAttack update", movement);
    }
  }
  return result;
}

}  // namespace kcds::game_client
