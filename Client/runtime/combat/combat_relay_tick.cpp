// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "combat_relay_tick.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>
#include <vector>

#include "../chat/chat_overlay.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../crafting/alchemy.hpp"
#include "../crafting/grinding.hpp"
#include "../crafting/smithing.hpp"
#include "../dice/dice_table.hpp"
#include "../knockout/knockout.hpp"
#include "../replicas/remote.hpp"
#include "../replicas/replica_removal.hpp"
#include "../riding/native_horses.hpp"
#include "../riding/trample.hpp"
#include "../stats/player_stats.hpp"
#include "../weapons/drawn_weapons.hpp"
#include "clinch_relay.hpp"
#include "combat_identity.hpp"
#include "combat_input.hpp"
#include "defence.hpp"
#include "defence_timing.hpp"
#include "dodge_relay.hpp"
#include "dodges.hpp"
#include "held_hits.hpp"
#include "melee_contacts.hpp"
#include "native_actions.hpp"
#include "native_effects.hpp"
#include "sync_attacks.hpp"

namespace kcds::game_client {

namespace integration = kcd2s::integration;
namespace native_chat = kcd2s::native_chat;

namespace {

// A relayed combat event to apply; for a contact on the local player, when it arrived and whether
// the dodger's screen answered it (dodges.hpp answer_contact).
struct Incoming {
  integration::MeleeEvent impact;
  std::optional<integration::HitVerdict> verdict;
  std::uint64_t arrived = 0;
  bool dodge_answered = false;
};
// Contacts on the local player waiting while a dodge window is open on it (engine thread).
std::vector<Incoming> dodge_waiting;

}  // namespace

// Every frame, after the replicas: the local fighter's target and model relayed, and the combat
// events other clients relayed (contacts, hits, reactions, clinches, dodges, defences, inputs,
// melee events) applied here.
void tick_combat_relay(const Frame& frame) {
  // The frame's values under the names frame() gave them.
  const auto own = frame.own;
  const auto& world = frame.world;
  const auto entity_system = frame.entity_system;
  const auto local_actor = frame.local_actor;
  const auto local_id = frame.local_id;
  relay_native_target(local_actor);
  relay_combat_model(local_actor);
  static void* previous_local_actor = nullptr;
  static unsigned previous_local_action = 28;
  auto local_movement = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 632);
  const auto local_action = native_action_type(local_movement);
  if (previous_local_actor != local_actor || previous_local_action != local_action) {
    const bool hit_started =
        previous_local_actor == local_actor && local_action == kNativeHitAction;
    previous_local_actor = local_actor;
    previous_local_action = local_action;
    log("COMBAT local native action type=%u paired=%u", local_action,
        unsigned(paired_action(local_action)));
    if (hit_started) {
      // Whichever path the game took the blow by (its own physical contact or the relayed attack
      // contact), the attacker's client gives this player's replica the same hit action.
      relay_combat_action({integration::CombatAction::HitAction, 1, 0});
    }
  }
  // Relayed impacts, and the contacts on the local player the server ruled on: those
  // come back with their verdict; a new one waits for its ruling.
  std::vector<Incoming> incoming;
  const std::uint64_t tick_ms = GetTickCount64();
  std::vector<integration::HitVerdict> hit_verdicts;
  bool hit_rules = false;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    for (auto& impact : client->peer().take_melee())
      incoming.push_back({std::move(impact), std::nullopt, tick_ms});
    hit_verdicts = client->peer().take_hit_verdicts();
    hit_rules = client->peer().hit_rules();
  }
  hits_ruled.store(hit_rules);
  rule_missiles(hit_verdicts);
  rule_priced_blows(hit_verdicts);
  for (auto& [impact, verdict] : take_ruled_contacts(hit_verdicts))
    incoming.push_back({std::move(impact), verdict, tick_ms});
  // The waiting contacts go round again, before this tick's own.
  incoming.insert(incoming.begin(), std::make_move_iterator(dodge_waiting.begin()),
                  std::make_move_iterator(dodge_waiting.end()));
  dodge_waiting.clear();
  for (auto& entry : incoming) {
    const auto& impact = entry.impact;
    const bool observer_contact =
        impact.contact && impact.attack_generation && impact.source != own && impact.target != own;
    // A blow between two other players: the victim's reaction (its game's evaluated hit).
    const bool observer_reaction = impact.reaction && impact.source != own && impact.target != own;
    if (impact.revision != world.revision ||
        (!observer_contact && !observer_reaction &&
         (impact.authority_is_target ? impact.source : impact.target) != own))
      continue;
    // A player in a dice match cannot be fought: a blow at them is
    // dropped before their game takes any health or plays a reaction, which broke the seated
    // minigame.
    if (impact.target == own && impact.source != own && kcds::runtime::dice::at_table()) {
      log("COMBAT MELEE dropped: the local player sits in a dice match (source=%u)", impact.source);
      continue;
    }
    // Nor while they smith, brew or sharpen: a hit broke the grindstone's
    // seated minigame the same way.
    if (impact.target == own && impact.source != own &&
        (kcds::runtime::crafting::smithing::own_running() ||
         kcds::runtime::crafting::alchemy::own_running() ||
         kcds::runtime::crafting::grinding::own_running())) {
      log("COMBAT MELEE dropped: the local player works at a crafting station (source=%u)",
          impact.source);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(network_mutex);
      if (!client->peer().current_combat_context(impact)) {
        log("COMBAT MELEE dropped stale context source=%u target=%u sequence=%llu", impact.source,
            impact.target, impact.sequence);
        continue;
      }
    }
    if (observer_contact) {
      const auto source = remotes.find(impact.source);
      const auto target = remotes.find(impact.target);
      if (source != remotes.end() && target != remotes.end() &&
          get_entity(entity_system, source->second.engine_id) &&
          get_entity(entity_system, target->second.engine_id)) {
        apply_observer_contact(impact, source->second.actor, source->second.engine_id,
                               target->second.actor, target->second.engine_id);
      }
      continue;
    }
    if (observer_reaction) {
      // The victim's replica takes the same native hit action once its owner's HitAction marker
      // says its game started one (give_replica_hit_action), with the attacker's replica as the
      // source. The engine ids are this client's, as in the attacker's own replay.
      const auto source = remotes.find(impact.source);
      const auto target = remotes.find(impact.target);
      if (source != remotes.end() && target != remotes.end() &&
          get_entity(entity_system, source->second.engine_id) &&
          get_entity(entity_system, target->second.engine_id)) {
        auto data = impact.reaction_data;
        std::memcpy(data.data(), &source->second.engine_id, 4);
        std::memcpy(data.data() + 40, &target->second.engine_id, 4);
        std::memcpy(data.data() + 72, &source->second.engine_id, 4);
        const std::uint64_t weapon = kcds::runtime::weapons::striking_item(source->second.actor);
        std::memcpy(data.data() + 8, &weapon, 8);
        auto source_move =
            *reinterpret_cast<void**>(static_cast<std::uint8_t*>(source->second.actor) + 632);
        auto target_move =
            *reinterpret_cast<void**>(static_cast<std::uint8_t*>(target->second.actor) + 632);
        auto& replica_hit = replica_hits[impact.target];
        replica_hit.reacted = GetTickCount64();
        replica_hit.data = data;
        replica_hit.attacker = impact.source;
        replica_hit.source_move = source_move;
        replica_hit.victim_move = target_move;
        give_replica_hit_action(impact.target, target_move);
      }
      continue;
    }
    auto attacker = remotes.find(impact.authority_is_target ? impact.target : impact.source);
    if (attacker == remotes.end() || !get_entity(entity_system, attacker->second.engine_id)) {
      log("COMBAT MELEE skipped missing source=%u sequence=%llu", impact.source, impact.sequence);
      continue;
    }
    auto source_actor = impact.authority_is_target ? local_actor : attacker->second.actor;
    auto target_actor = impact.authority_is_target ? attacker->second.actor : local_actor;
    auto source_engine = impact.authority_is_target ? local_id : attacker->second.engine_id;
    auto target_engine = impact.authority_is_target ? attacker->second.engine_id : local_id;
    auto source_soul =
        *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(source_actor) + 1640);
    auto target_soul =
        *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(target_actor) + 1640);
    if (impact.attack_generation_marker) {
      auto movement = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(source_actor) + 632);
      const auto scope = replica_contact_scope(impact.source, source_actor, movement);
      if (scope) {
        replica_contacts[impact.source].announce(*scope, impact.attack_generation,
                                                 impact.native_attack_kind);
      }
      continue;
    }
    if (impact.sync_hit) {
      pending_sync_hits[impact.source] = {impact.sync_hit_data, GetTickCount64()};
      log("COMBAT SyncHit owner selection received source=%u target=%u sequence=%llu",
          impact.source, impact.target, impact.sequence);
      continue;
    }
    if (impact.contact) {
      auto source_move =
          *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(source_actor) + 632);
      auto target_move =
          *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(target_actor) + 632);
      if (!source_move || !target_move || target_actor != local_actor)
        continue;
      auto handler = *reinterpret_cast<void**>(source_move + 904);
      if (!handler)
        continue;
      // A player struck while locked on nobody locks on the striker, as the game locks on an NPC
      // that attacks: the game's own target setter and its hard-lock byte, as
      // the lock key sets them, and the short window in which the hit reaction's clear is skipped.
      auto local_core = *reinterpret_cast<std::uint8_t**>(target_move + 528);
      if (local_core && !*reinterpret_cast<void**>(local_core + 3320)) {
        void* striker = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(source_actor) + 56);
        hooked_set_combat_target(target_move, striker);
        if (auto controller = *reinterpret_cast<std::uint8_t**>(target_move + 1104)) {
          controller[kHardLockFlag] = 1;
        }
        keep_hard_lock.store(true);
        keep_lock_entity.store(striker);
        keep_lock_until.store(GetTickCount64() + kKeepLockMs);
        log("COMBAT TARGET auto lock on striker %u", impact.source);
      }
      // The dodger's own screen answers the blow first.
      if (!entry.dodge_answered) {
        namespace combat = kcds::runtime::combat;
        const auto answer = combat::answer_contact(tick_ms, entry.arrived, local_slot_dodge_at(),
                                                   local_dodge_windows(local_actor));
        if (answer == combat::DodgeAnswer::Wait) {
          dodge_waiting.push_back(entry);
          continue;
        }
        entry.dodge_answered = true;
        if (answer == combat::DodgeAnswer::Dodged) {
          log("COMBAT DODGE relayed blow source=%u dodged on this screen (%llu ms after it came)",
              impact.source, static_cast<unsigned long long>(tick_ms - entry.arrived));
          continue;
        }
      }
      // A blow from another player waits for the server's ruling while some resource handles
      // playerHit: the game works it out at once and only its health waits (see
      // PricedBlow); without the pricing hooks the whole blow waits before it is worked out.
      if (!entry.verdict && hit_rules && !priced_hits_ready) {
        if (!hold_contact(impact)) {
          log("HIT contact dropped: ruling queue unavailable source=%u", impact.source);
        }
        continue;
      }
      auto ruled_soul =
          *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(local_actor) + 1640);
      const float health_before = ruled_soul ? *reinterpret_cast<float*>(ruled_soul + 1800) : 0;
      // The health the server ruled: the game works the blow out and takes its
      // health; a ruling that sets or scales it puts the difference right once it is done.
      const auto take_ruling = [&]() {
        if (!active_ruling) {
          return;
        }
        const ActiveRuling ruling = *active_ruling;
        active_ruling.reset();
        if (!ruling.queued || !ruled_soul) {
          log("HIT %u ruled, but the game worked out no blow", ruling.verdict.request);
          return;
        }
        if (ruling.ruled != ruling.worked_out) {
          integration::StatRequest health;
          health.stat = 0;
          health.value =
              std::max(integration::kMinPositiveHealth * 2, health_before - ruling.ruled);
          kcds::runtime::stats::apply(ruled_soul, {health}, kcds::runtime::knockout::knocked_out());
        }
        log("HIT %u taken as ruled: the game worked out %.2f health, ruled %.2f",
            ruling.verdict.request, ruling.worked_out, ruling.ruled);
      };
      alignas(16) integration::NativeReactionData hit{};
      std::copy_n(impact.contact_data.begin(), 160, hit.begin());
      alignas(16) std::array<std::uint8_t, 144> attack{};
      std::copy_n(impact.contact_data.begin() + 160, 144, attack.begin());
      std::memcpy(attack.data() + 8, &source_engine, 4);
      std::memcpy(attack.data() + 36, &target_engine, 4);
      alignas(16) std::array<std::uint8_t, 64> context{};
      std::copy_n(impact.contact_data.begin() + 304, 16, context.begin());
      // Physical (unsynchronized) contacts are detected by the attacker's owner and relayed here
      // a replica's own collision with the local player is ignored, so each
      // contact is applied once, by this original native callback.
      // MCP loads modifier records as aligned __m128 values.
      // The compact wire offset is deliberately not a native SIMD address.
      alignas(16) std::array<std::uint8_t, 128> native_modifiers{};
      auto count = impact.contact_data[320];
      std::copy_n(impact.contact_data.begin() + 321, count * 16, native_modifiers.begin());
      auto modifiers = native_modifiers.data();
      // Original725DAC must populate this fresh HitInfo, including outcome/KO.
      // Its source==0 sentinel was preserved by portable_reaction_data.
      *reinterpret_cast<void**>(context.data() + 16) = target_move;
      *reinterpret_cast<void**>(context.data() + 24) = attack.data();
      *reinterpret_cast<const void**>(context.data() + 32) = modifiers;
      *reinterpret_cast<const void**>(context.data() + 40) = modifiers + count * 16;
      *reinterpret_cast<const void**>(context.data() + 48) = modifiers + count * 16;
      float power;
      unsigned state;
      std::memcpy(&power, impact.contact_data.data() + 449, 4);
      std::memcpy(&state, impact.contact_data.data() + 453, 4);
      if (state == *native_backoff_attack_type)
        continue;
      if (entry.verdict) {
        active_ruling = ActiveRuling{*entry.verdict};
      }
      if (!context[0]) {
        void* listener = physical_contact_listener(source_move);
        if (listener) {
          alignas(16) std::array<std::uint8_t, kAttackRecordBytes> record{};
          std::copy(attack.begin(), attack.end(), record.begin());
          *reinterpret_cast<void**>(record.data() + kAttackVictimMovement) = target_move;
          engine_stage = "native physical contact replay";
          replaying_physical_contact = true;
          physical_contact_evaluated = false;
          native_physical_contact(listener, record.data());
          replaying_physical_contact = false;
          log("COMBAT CONTACT native physical replay source=%u target=%u sequence=%llu "
              "evaluated=%u",
              impact.source, impact.target, impact.sequence, unsigned(physical_contact_evaluated));
          if (physical_contact_evaluated) {
            take_ruling();
            continue;
          }
        }
      }
      char native_flag = 0;
      std::array<std::uint8_t, 13> outcome{};
      std::copy_n(impact.contact_data.begin() + 457, 13, outcome.begin());
      engine_stage = "native owner contact evaluation";
      auto physical_core = *reinterpret_cast<void**>(source_move + 528);
      prepare_native_contact_target(physical_core, target_move);
      const auto produced = produce_native_hit(handler, hit.data(), &power, &native_flag,
                                               context.data(), true, state, outcome.data());
      if (!context[0]) {
        finish_native_physical_contact(source_move, physical_core, power, native_flag);
      }
      if (produced) {
        // D53778/D5235C both clear HitInfo+120 bit0x20 for an actor
        // target before GameRules v165. D542E0 owns the combat reaction;
        // leaving this bit set queues a second actor reaction from B24600.
        auto flags = *reinterpret_cast<unsigned*>(hit.data() + 120);
        // D53778: before dispatch, source stance4/8 (unless4096)
        // clears bit2 and marks +117. Without this native branch, SyncHit's
        // impact listener interrupts its own pair and Player feedback unlocks.
        // IDA688B44 reads this same native getter at core+2976/v1.
        auto source_core = *reinterpret_cast<std::uint8_t**>(source_move + 528);
        const auto stance =
            slot<unsigned(__fastcall*)(void*)>(source_core + 2976, 1)(source_core + 2976);
        // The victim in combat gets the combat module's hit action (see start_native_hit_action):
        // for a physical contact unless its current action handles the hit itself,
        // for an attack when the hit is a combat hit (+64/+65) or the source's stance is 4096
        auto target_soul_core = *reinterpret_cast<std::uint8_t**>(target_move + 528);
        const bool victim_in_combat =
            target_soul_core &&
            slot<char(__fastcall*)(void*)>(target_soul_core, 1)(target_soul_core);
        bool hit_action = false;
        if (victim_in_combat && !(stance & (4u | 8u) && !(stance & 4096u))) {
          const bool physical = !context[0];
          const bool wanted = physical ? !(action_handles_hit && action_handles_hit(target_move))
                                       : ((stance & 4096u) || hit[64] || hit[65]);
          if (wanted) {
            engine_stage = "native combat hit action";
            hit_action = start_native_hit_action(source_move, target_move, hit.data(), stance);
          }
          log("COMBAT CONTACT hit action physical=%u wanted=%u started=%u combat_hit=%u/%u",
              unsigned(physical), unsigned(wanted), unsigned(hit_action), unsigned(hit[64]),
              unsigned(hit[65]));
        }
        if (hit_action) {
          flags &= ~2u;
          hit[117] = 1;
        } else if (!(stance & 4096u) && (stance & (4u | 8u))) {
          flags &= ~2u;
          hit[117] = 1;
          auto helper = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(target_actor) + 640);
          // Live RPM and identify original ActorData v11=.
          // Keep the same native callback, never write helper state directly.
          if (helper)
            slot<void(__fastcall*)(void*, char)>(helper, 11)(helper, 1);
        } else if (!(stance & 4096u) && !hit[64] && !hit[65]) {
          // D53778's next branch clears flag0x2 even when the native core+2976 flag is zero,
          // when A73F78 (the attacker's primary action is 22 or 9), (the attacker's
          // clinch state is a clinch state) or D55EB8 (the victim's core + 1664 bool) holds.
          // IDA1816EAC58 proves this is GameRules' animation-reaction gate:
          // B24600 treats cleared0x2 as already handled, preserving the target
          // and its native SyncHit instead of selecting a second hit animation.
          // The relayed contact of a clinch attack can come after the attacker's replica here left
          // its SyncAttack for the pair's transition (source_action 24, clinch state
          // 25): without the clinch-state test the victim then played a second hit reaction,
          // which cut its transition and dropped its lock.
          constexpr std::size_t kCoreVictimSynced = 1664;
          const auto source_action = native_action_type(source_move);
          const bool source_paired = source_action == 22 || source_action == 9;
          bool source_in_clinch = false;
          if (native_in_clinch_state) {
            source_in_clinch = native_in_clinch_state(source_move);
          }
          bool victim_synced = false;
          if (target_soul_core) {
            auto synced = target_soul_core + kCoreVictimSynced;
            victim_synced = slot<char(__fastcall*)(void*)>(synced, 1)(synced) != 0;
          }
          if (source_paired || source_in_clinch || victim_synced) {
            flags &= ~2u;
          }
          log("COMBAT CONTACT reaction gate paired=%u clinch=%u synced=%u", unsigned(source_paired),
              unsigned(source_in_clinch), unsigned(victim_synced));
        }
        // A relayed physical contact on a fighting victim: the game plays its own
        // hit reaction (flag 0x2 stays). During that reaction its per-frame target selection
        // finds no target, clears the target and drops the hard lock (the
        // combat controller's byte +192, movement+1104). For a short window the clear is
        // skipped and the hard lock is kept (with the reaction marked handled the lock
        // held but no hit animation played).
        auto target_core = *reinterpret_cast<std::uint8_t**>(target_move + 528);
        const bool victim_locked =
            target_core && *reinterpret_cast<void**>(target_core + 3320) != nullptr;
        if (!context[0] && victim_locked) {
          auto controller = *reinterpret_cast<std::uint8_t**>(target_move + 1104);
          keep_hard_lock.store(controller && controller[kHardLockFlag] != 0);
          keep_lock_entity.store(
              *reinterpret_cast<void**>(static_cast<std::uint8_t*>(source_actor) + 56));
          keep_lock_until.store(GetTickCount64() + kKeepLockMs);
        }
        flags &= ~0x20u;
        std::memcpy(hit.data() + 120, &flags, 4);
        log("COMBAT CONTACT native completion core_flags=%x flags=%x preserved=%u source_action=%u "
            "target_action=%u",
            stance, flags, unsigned(hit[117]), native_action_type(source_move),
            native_action_type(target_move));
        auto rules = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(get_services()) + 416);
        if (rules)
          slot<void(__fastcall*)(void*, const void*)>(rules, 165)(rules, hit.data());
        hooked_hit_reaction(source_move, hit.data(), target_move, outcome.data());
      }
      take_ruling();
      // The native screen blood is drawn under a full-screen game UI; show our own vignette.
      if (produced && local_full_screen_ui())
        native_chat::flash_damage();
      log("COMBAT CONTACT owner source=%u target=%u sequence=%llu produced=%u", impact.source,
          impact.target, impact.sequence, unsigned(produced));
      continue;
    }
    if (impact.input && impact.input_data.action == integration::CombatAction::Clinch) {
      engine_stage = "native relayed clinch";
      apply_relayed_clinch(impact.source, attacker->second.actor, local_actor, impact.input_data);
      continue;
    }
    if (impact.input) {
      const auto tracked_contact = replica_contacts.find(impact.source);
      const bool attack_input =
          impact.input_data.action == integration::CombatAction::AttackMouse ||
          impact.input_data.action == integration::CombatAction::Attack ||
          impact.input_data.action == integration::CombatAction::Special ||
          impact.input_data.action == integration::CombatAction::Abort;
      auto movement = *reinterpret_cast<std::uint8_t**>(
          static_cast<std::uint8_t*>(attacker->second.actor) + 632);
      auto controller = movement ? *reinterpret_cast<std::uint8_t**>(movement + 1104) : nullptr;
      auto native_input = controller ? *reinterpret_cast<std::uint8_t**>(controller + 40) : nullptr;
      if (!native_input || *reinterpret_cast<void**>(native_input + 24) != attacker->second.actor) {
        if (attack_input && tracked_contact != replica_contacts.end()) {
          tracked_contact->second.finish_input();
        }
        if (attack_input) {
          refused_attack_inputs.erase(impact.source);
          riposte_attack_inputs.erase(impact.source);
        }
        continue;
      }
      const auto& combat_action = impact.input_data;
      // A mounted rider steers its horse with the movement keys; replayed into the replica's
      // on-foot combat input they pulled the mounted replica out of its riding pose (the
      // replica stood on the horse). The horse itself is replicated.
      const bool steering = combat_action.action == integration::CombatAction::MoveLeft ||
                            combat_action.action == integration::CombatAction::MoveRight ||
                            combat_action.action == integration::CombatAction::MoveForward ||
                            combat_action.action == integration::CombatAction::MoveBack;
      if (steering && (native_rider_transition(attacker->second.actor) ||
                       native_human_mounted(attacker->second.actor))) {
        continue;
      }
      if (combat_action.action == integration::CombatAction::AttackZone) {
        // The zone object at core+384 is a vtable followed by the zone value (words 2 equals
        // the slot-1 getter result). Write the attacker's zone only when the replica's object has
        // the same class as the local player's, so an unexpected layout is never written.
        auto replica_core = *reinterpret_cast<std::uint8_t**>(movement + 528);
        auto local_zone_movement =
            *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(local_actor) + 632);
        auto local_core = local_zone_movement
                              ? *reinterpret_cast<std::uint8_t**>(local_zone_movement + 528)
                              : nullptr;
        if (replica_core && local_core &&
            *reinterpret_cast<void**>(replica_core + 384) ==
                *reinterpret_cast<void**>(local_core + 384)) {
          *reinterpret_cast<std::uint32_t*>(replica_core + 392) =
              static_cast<std::uint32_t>(combat_action.value);
        } else {
          log("COMBAT attack zone not applied source=%u", impact.source);
        }
        continue;
      }
      if (combat_action.action == integration::CombatAction::DefenceTiming) {
        defence_timings[impact.source] = {ULONGLONG(combat_action.value), GetTickCount64()};
      } else if (combat_action.action == integration::CombatAction::AttackRefused) {
        refused_attack_inputs.insert(impact.source);
      } else if (combat_action.action == integration::CombatAction::Riposte) {
        riposte_attack_inputs.insert(impact.source);
      } else if (combat_action.action == integration::CombatAction::HitAction) {
        replica_hits[impact.source].marked = GetTickCount64();
        give_replica_hit_action(impact.source, movement);
      } else if (combat_action.action == integration::CombatAction::Dodge) {
        engine_stage = "native replica dodge";
        start_replica_dodge(impact.source, combat_action.value);
      } else if (combat_action.action == integration::CombatAction::DefenceGuard) {
        relayed_defence_guards[impact.source] = unsigned(combat_action.value);
      } else if (combat_action.action == integration::CombatAction::DefenceMode) {
        relayed_defence_modes[impact.source] = unsigned(combat_action.value);
      } else if (combat_action.action == integration::CombatAction::CombatModel) {
        auto [model, added] = owner_models.try_emplace(impact.source);
        if (added) {
          model->second = {~0u, ~0u, ~0u};
        }
        const std::size_t index = combat_action.mode - 1u;
        model->second[index] = unsigned(combat_action.value);
        const std::size_t property = kCoreModelProperties[index];
        // A clinch held here keeps its own clinch state: the owner's next one (sent right after
        // the guard that starts its transition) must not reach the replica before the replica's
        // own transition reads the state it leaves.
        bool take = true;
        if (property == kCoreGuard) {
          take = !native_clinch_owns_guard(movement) ||
                 owner_guard_for_held_clinch(movement, model->second);
        } else if (property == kCoreMode) {
          take = !has_guard_sync_movement(movement);
        }
        if (take) {
          take_owner_property(movement, property, model->second[index]);
        }
        if (property != kCoreStance && has_guard_sync_movement(movement)) {
          log("COMBAT clinch owner model source=%u property=%zu value=%u owner_state=%d state=%d "
              "taken=%u",
              impact.source, property, model->second[index], int(model->second[2]),
              combat_property_value(movement, kCoreMode), unsigned(take));
        }
      } else if (combat_action.action == integration::CombatAction::PerfectBlock) {
        engine_stage = "native replica perfect block";
        start_replica_perfect_block(impact.source, movement, unsigned(combat_action.value),
                                    kPerfectBlockKind,
                                    perfect_block_request ? *perfect_block_request : 0);
      } else if (combat_action.action == integration::CombatAction::SyncPerfectBlock) {
        engine_stage = "native replica master strike";
        // Neither start has a variant here (passes 0: any attacker's window; the
        // owner's pair perfect blocks came with 0 too, live).
        const unsigned request = unsigned(combat_action.value);
        start_replica_perfect_block(
            impact.source, movement, 0,
            combat_action.mode == 1 ? kMasterStrikeKind : kPairPerfectBlockKind,
            request < kNoMasterStrikeRequest ? request : 0xFFFFFFFFu);
      } else if (combat_action.action == integration::CombatAction::Target ||
                 combat_action.action == integration::CombatAction::TargetOther) {
        // Target mode 1: the lock is on the local player; mode 2: not on it; TargetOther (after
        // mode 2): on that other player, whose replica the watcher's replica locks onto.
        core::EntityId selected = 0;
        if (combat_action.action == integration::CombatAction::TargetOther) {
          selected = core::EntityId(combat_action.value);
        } else if (combat_action.mode == 1) {
          selected = local_network_id.load();
        }
        {
          std::lock_guard<std::mutex> lock(combat_identity_mutex);
          remote_native_targets[source_soul] = selected;
        }
        engine_stage = "native replica target selection";
        // The hook replaces the entity with the replica's own selection.
        hooked_set_combat_target(movement, nullptr);
      } else {
        // Registry offsets verified live and in the original input constructor.
        static const unsigned offsets[] = {0,   296, 256, 272, 264, 240, 312, 288,
                                           320, 328, 0,   8,   16,  24,  32,  280};
        auto registry =
            *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(get_services()) + 48);
        engine_stage = "native replica combat input";
        replaying_combat_input = true;
        auto previous_source = replayed_combat_movement;
        replayed_combat_movement = movement;
        auto previous_intent = remote_manual_combat_soul;
        const bool original_intent =
            combat_action.action == integration::CombatAction::AttackMouse ||
            combat_action.action == integration::CombatAction::Attack ||
            combat_action.action == integration::CombatAction::Prepare ||
            combat_action.action == integration::CombatAction::Block ||
            combat_action.action == integration::CombatAction::Lock ||
            combat_action.action == integration::CombatAction::Special;
        if (original_intent)
          remote_manual_combat_soul = source_soul;
        const bool attack_press = (combat_action.action == integration::CombatAction::AttackMouse ||
                                   combat_action.action == integration::CombatAction::Attack) &&
                                  (combat_action.mode == 1 || combat_action.mode == 2);
        const auto attack_registry = registry + offsets[unsigned(combat_action.action)];
        auto replica_core = *reinterpret_cast<std::uint8_t**>(movement + 528);
        if (attack_press && riposte_attack_inputs.erase(impact.source) > 0) {
          // Its owner's game asked for the riposte with this input: the replica's game asks
          // for the same one through the game's own setter of the request, whatever the replica's
          // window shows now, and does not take the input. Only in the replica's perfect block,
          // where the owner's was: the riposte goes on from it.
          refused_attack_inputs.erase(impact.source);
          dropped_attack_presses.erase(impact.source);
          const unsigned replica_action = native_action_type(movement);
          const bool asked =
              set_bool_property && replica_core && replica_action == kNativePerfectBlockAction;
          if (asked) {
            set_bool_property(replica_core + kCoreRiposteRequest, 1);
          }
          if (tracked_contact != replica_contacts.end()) {
            tracked_contact->second.finish_input();
          }
          remote_manual_combat_soul = previous_intent;
          replayed_combat_movement = previous_source;
          replaying_combat_input = false;
          log("COMBAT replica riposte %s as by its owner's game source=%u mode=%u action=%u "
              "window=%u",
              asked ? "asked" : "not asked (not in its perfect block)", impact.source,
              unsigned(combat_action.mode), replica_action,
              unsigned(riposte_window_open(attacker->second.actor)));
          continue;
        }
        if (attack_press && refused_attack_inputs.erase(impact.source) > 0) {
          // Its owner's game made nothing of this press or release (no attack, no riposte): the
          // replica's game does not get it either. Replayed, a press during the replica's own
          // parry turned into a riposte or an attack its owner never made (parry and
          // counter loop). A dropped press is replayed just before a release its owner's game
          // did make an attack of, so the buttons stay paired.
          dropped_attack_presses[impact.source] =
              combat_action.mode == 1 ? attack_registry : nullptr;
          if (tracked_contact != replica_contacts.end()) {
            tracked_contact->second.finish_input();
          }
          remote_manual_combat_soul = previous_intent;
          replayed_combat_movement = previous_source;
          replaying_combat_input = false;
          log("COMBAT replica attack input left out as by its owner's game source=%u mode=%u",
              impact.source, unsigned(combat_action.mode));
          continue;
        }
        if (attack_press && set_bool_property && replica_core &&
            riposte_window_open(attacker->second.actor)) {
          // Its owner's game asked for no riposte with this input: the replica's window, open at
          // its own moment, is closed through the game's setter first, so the replica's game takes
          // the input as its owner's did (at the window's edge the replica riposted where its
          // owner's game took the press as an attack).
          set_bool_property(replica_core + kCoreRiposteWindow, 0);
          log("COMBAT replica riposte window closed: no riposte by its owner's game source=%u "
              "mode=%u",
              impact.source, unsigned(combat_action.mode));
        }
        if (attack_press && combat_action.mode == 2) {
          const auto dropped = dropped_attack_presses.find(impact.source);
          if (dropped != dropped_attack_presses.end()) {
            if (dropped->second) {
              replaying_refused_press = true;
              original_combat_input(native_input, dropped->second, 1, 1.0f);
              replaying_refused_press = false;
            }
            dropped_attack_presses.erase(dropped);
          }
        } else if (attack_press) {
          dropped_attack_presses.erase(impact.source);
        }
        original_combat_input(native_input, attack_registry, combat_action.mode,
                              combat_action.value);
        if (attack_input && tracked_contact != replica_contacts.end()) {
          tracked_contact->second.finish_input();
        }
        remote_manual_combat_soul = previous_intent;
        replayed_combat_movement = previous_source;
        replaying_combat_input = false;
        if (combat_action.action == integration::CombatAction::Block && combat_action.mode == 1) {
          start_defence_catch_up(impact.source);
        }
        // A pair the owner's release chose and this replica's release did not make is dropped, so
        // no later input of the replica makes it. F (attack_special) releases its push this way
        // too: on a screen where the replica could not push, the push selection stayed and
        // the replica's next attack input within 2 s could make the push instead.
        if (combat_action.mode == 2 &&
            (combat_action.action == integration::CombatAction::AttackMouse ||
             combat_action.action == integration::CombatAction::Attack ||
             combat_action.action == integration::CombatAction::Special)) {
          if (pending_sync_hits.erase(impact.source))
            log("COMBAT native pair selection unused by native release source=%u action=%u",
                impact.source, unsigned(combat_action.action));
        }
      }
      if (combat_action.action != integration::CombatAction::ZoneX &&
          combat_action.action != integration::CombatAction::ZoneY)
        log("COMBAT ACTION applied source=%u target=%u action=%u mode=%u sequence=%llu "
            "native_input=%p",
            impact.source, impact.target, unsigned(combat_action.action),
            unsigned(combat_action.mode), impact.sequence, native_input);
      continue;
    }
    auto rpg_module = static_cast<std::uint8_t*>(*rpg_module_slot);
    auto world_events = rpg_module ? *reinterpret_cast<void**>(rpg_module + 176) : nullptr;
    if (!source_soul || !target_soul || !world_events)
      continue;
    if (impact.reaction) {
      {
        const auto& r = impact.reaction_data;
        log("COMBAT REACTION bytes hit64=%u hit65=%u flags=%02x%02x%02x%02x outcome=%02x %02x %02x "
            "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            r[64], r[65], r[123], r[122], r[121], r[120], r[160], r[161], r[162], r[163], r[164],
            r[165], r[166], r[167], r[168], r[169], r[170], r[171], r[172]);
      }
      // The victim's answer. A hit: the local swing held at the contact goes on. A block or
      // parry (the outcome's defence bytes +1 and +3, live: 01 and 01; a hit leaves them 0): the
      // swing stays at the blades until the game starts the attacker's deflection clip below, so
      // it does not run on through the defender's weapon first.
      if (swing_hold.skeleton && impact.target == swing_hold.victim) {
        combat_round_trip_ms = GetTickCount64() - swing_hold.started;
        const bool defended = impact.reaction_data[161] || impact.reaction_data[163];
        if (defended) {
          swing_hold.until = GetTickCount64() + kDeflectionWaitMs;
          swing_hold.awaiting_deflection = true;
          log("COMBAT SWING kept at the blades until the deflection");
        } else {
          release_local_swing(true);
        }
      }
      // A contact preview is not an applied reaction. The local-source hook suppressed its
      // original movement-signal dispatch, so the owner's result must reach the native
      // source/victim callback below, including a block or parry that the preview anticipated.
      // Reaction receipt deduplication belongs to the scoped reliable combat stream; a recent
      // preview against the same target does not identify this authoritative contact.
      auto data = impact.reaction_data;
      std::memcpy(data.data(), &source_engine, 4);
      std::memcpy(data.data() + 40, &target_engine, 4);
      std::memcpy(data.data() + 72, &source_engine, 4);
      // The weapon it was struck with, the attacker's own here: without it the game took a sword
      // blow for a fist's and played a blunt impact sound.
      const std::uint64_t weapon = kcds::runtime::weapons::striking_item(source_actor);
      std::memcpy(data.data() + 8, &weapon, 8);
      auto source_move = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(source_actor) + 632);
      auto target_move = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(target_actor) + 632);
      if (!source_move || !target_move)
        continue;
      engine_stage = "native hit reaction replay";
      replaying_melee = true;
      if (*reinterpret_cast<const unsigned*>(data.data() + 24) == kCollisionHit) {
        // A horse trample: the collision HitInfo goes through GameRules only; it has
        // no melee outcome for the combat reaction dispatch. Its source is the horse the rider
        // sits on (the native collision damage depends on the source; live: with the rider as
        // source it took about 1 health).
        const unsigned horse_engine = ridden_horse_engine(impact.source);
        if (horse_engine) {
          std::memcpy(data.data(), &horse_engine, 4);
          std::memcpy(data.data() + 72, &horse_engine, 4);
        }
        // The damage is the game's own collision result from the rider's client; the HitInfo
        // then plays the collision reaction.
        const float health_before = *reinterpret_cast<float*>(target_soul + 1800);
        clip_trace_until.store(GetTickCount64() + kTrampleFallMs);
        const bool native_event =
            apply_trample_damage(data, ridden_horse_actor(impact.source), target_actor);
        auto rules = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(get_services()) + 416);
        if (rules)
          slot<void(__fastcall*)(void*, const void*)>(rules, 165)(rules, data.data());
        replaying_melee = false;
        log("COMBAT TRAMPLE applied source=%u target=%u sequence=%llu horse=%u native_damage=%u "
            "health_cache=%.5f->%.5f",
            impact.source, impact.target, impact.sequence, horse_engine, unsigned(native_event),
            health_before, *reinterpret_cast<float*>(target_soul + 1800));
        continue;
      }
      // The source caller was suppressed until this target-owner result.
      // Restore its original pair feedback callback from native HitInfo+117.
      if (data[117] && native_action_type(source_move) == 22 &&
          native_action_type(target_move) == 23) {
        auto helper = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(target_actor) + 640);
        if (helper)
          slot<void(__fastcall*)(void*, char)>(helper, 11)(helper, 1);
      }
      // MCP: GameRules v165 queues HitInfo; its next native update
      // calls -> GameObject event 32 ->. That invokes
      // C_Player v225 and its native first-person feedback.
      auto rules = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(get_services()) + 416);
      if (rules)
        slot<void(__fastcall*)(void*, const void*)>(rules, 165)(rules, data.data());
      auto previous_observed = observed_reaction_movement;
      observed_reaction_movement = target_move;
      // The game's own block or parry clips go on the defender's replica now, at the moment the
      // swords meet here, instead of waiting for its owner's clips; traced for a moment.
      clip_trace_until.store(GetTickCount64() + 800);
      open_reaction_window(impact.source);
      open_reaction_window(impact.target);
      replaying_animation = true;
      original_hit_reaction(source_move, data.data(), target_move, data.data() + 160);
      replaying_animation = false;
      observed_reaction_movement = previous_observed;
      replaying_melee = false;
      log("COMBAT REACTION applied source=%u target=%u sequence=%llu", impact.source, impact.target,
          impact.sequence);
      if (target_actor != local_actor) {
        auto& replica_hit = replica_hits[impact.target];
        replica_hit.reacted = GetTickCount64();
        replica_hit.data = data;
        replica_hit.attacker = impact.source;
        replica_hit.source_move = source_move;
        replica_hit.victim_move = target_move;
        give_replica_hit_action(impact.target, target_move);
      }
      continue;
    }
    auto cause = impact.data;
    if (impact.effects && !resolve_committed_equipment(cause, source_actor, target_actor)) {
      log("COMBAT EFFECTS skipped unavailable equipment source=%u target=%u sequence=%llu",
          impact.source, impact.target, impact.sequence);
      continue;
    }
    integration::melee_set_u64(cause, 0, *reinterpret_cast<std::uint64_t*>(source_soul + 48));
    integration::melee_set_u64(cause, 16, *reinterpret_cast<std::uint64_t*>(target_soul + 48));
    std::memcpy(cause.data() + 8, &source_engine, 4);
    std::memcpy(cause.data() + 24, &target_engine, 4);
    auto executor_soul = impact.executor_is_target ? target_soul : source_soul;
    auto combat_soul = slot<void*(__fastcall*)(void*)>(executor_soul, 57)(executor_soul);
    if (!combat_soul)
      continue;
    engine_stage = "native melee RPG replay";
    const float before = *reinterpret_cast<float*>(target_soul + 1800);
    void* event = nullptr;
    replaying_melee = true;
    if (impact.effects) {
      void* native_cause = nullptr;
      create_native_cause(&native_cause, cause.data());
      if (native_cause)
        create_native_event(nullptr, &event, &native_cause);
      if (native_cause)
        slot<void*(__fastcall*)(void*, char)>(native_cause, 0)(native_cause, 1);
      if (event && !populate_native_effects(event, impact.effect_data,
                                            *reinterpret_cast<std::uint64_t*>(source_soul + 48),
                                            *reinterpret_cast<std::uint64_t*>(target_soul + 48),
                                            *reinterpret_cast<std::uint64_t*>(source_soul + 48))) {
        slot<void(__fastcall*)(void*)>(event, 4)(event);
        event = nullptr;
        log("COMBAT EFFECTS native allocation failed; event not submitted");
      }
    } else {
      // Legacy target-authority cause packets must not recompute damage against
      // a replica's different RPG stats. Current native commits carry explicit effects in KCM8.
      log("COMBAT MELEE legacy cause ignored; committed effects required");
    }
    if (event) {
      // The same borrowed-event Submit + Release sequence as native scripted-hit.
      auto previous_committed = committed_replay_event;
      committed_replay_event = impact.effects ? event : nullptr;
      slot<void(__fastcall*)(void*, void*)>(world_events, 0)(world_events, event);
      committed_replay_event = previous_committed;
      slot<void(__fastcall*)(void*)>(event, 4)(event);
    }
    replaying_melee = false;
    log("COMBAT MELEE applied source=%u target=%u sequence=%llu native_event=%u "
        "health_cache=%.5f->%.5f",
        impact.source, impact.target, impact.sequence, unsigned(event != nullptr), before,
        *reinterpret_cast<float*>(target_soul + 1800));
  }
}

}  // namespace kcds::game_client
