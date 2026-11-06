// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "melee_contacts.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/remote.hpp"
#include "../weapons/drawn_weapons.hpp"
#include "combat_identity.hpp"
#include "combat_input.hpp"
#include "defence.hpp"
#include "native_effects.hpp"
#include "player_damage.hpp"
#include "reaction_window.hpp"
#include "replica_contact.hpp"
#include "sync_attacks.hpp"

#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// MCP: C_CombatSoul v42 creates a ref-counted C_Event from the 144-byte
// value cause. C_RPGWorld v0 submits it; native Release drops our event reference.
MeleeHit original_melee_hit;
void** rpg_module_slot;
thread_local bool replaying_melee = false;
std::mutex pending_melee_mutex;
local::NativeEventRelay pending_melee;
// IDA9DB51C always invokes modifiers9DB800 before original Apply9DB7A4.
// Incoming KCM8 (or legacy KCM6) is already post-modifier committed data. Preserve it exactly;
// original Submit still applies/publishes/releases the event through the engine.
ModifyNativeEvent original_modify_native_event = nullptr;
thread_local void* committed_replay_event = nullptr;
void __fastcall hooked_modify_native_event(void* event) {
  if (event == committed_replay_event)
    return;
  original_modify_native_event(event);
}
SubmitRpgEvent original_submit_rpg_event;
DestroyRpgEvent original_destroy_rpg_event;
char __fastcall hooked_submit_rpg_event(void* self, void* event) {
  std::optional<integration::MeleeEvent> impact;
  {
    std::lock_guard<std::mutex> lock(pending_melee_mutex);
    impact = pending_melee.submit(reinterpret_cast<std::uintptr_t>(event));
  }
  const bool committed_here =
      impact && !replaying_melee && impact->revision == requested_revision &&
      (impact->authority_is_target ? impact->target : impact->source) == local_network_id.load();
  std::uint64_t source_soul = 0;
  integration::NativeEffects attacker_wear;
  if (committed_here) {
    {
      std::lock_guard<std::mutex> identities(combat_identity_mutex);
      for (const auto& entry : remote_combat_souls)
        if (entry.second == impact->source)
          source_soul =
              *reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(entry.first) + 48);
    }
    attacker_wear = take_attacker_wear(event, source_soul);
  }
  const auto result = original_submit_rpg_event(self, event);
  if (committed_here) {
    bool sent;
    {
      std::lock_guard<std::mutex> lock(network_mutex);
      auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
      auto local_soul = actor ? *reinterpret_cast<std::uint8_t**>(actor + 1640) : nullptr;
      auto effects =
          local_soul ? capture_native_effects(event, source_soul,
                                              *reinterpret_cast<std::uint64_t*>(local_soul + 48))
                     : integration::NativeEffects{};
      for (const auto& wear : attacker_wear) {
        if (effects.size() < 16) {
          effects.push_back(wear);
        }
      }
      sent = client->peer().send_native_effects(impact->source, impact->target, impact->data,
                                                effects, impact->executor_is_target);
      if (!sent) {
        log("COMBAT EFFECTS rejected cause_valid=%u effects_valid=%u world_ready=%u",
            unsigned(integration::valid_committed_melee_data(impact->data)),
            unsigned(integration::valid_native_effects(effects)),
            unsigned(client->peer().world_ready()));
        for (const auto& effect : effects) {
          unsigned attribute = 0;
          float delta = 0;
          std::memcpy(&attribute, effect.value.data(), 4);
          std::memcpy(&delta, effect.value.data() + 4, 4);
          log("COMBAT EFFECTS rejected record kind=%u role=%u attribute=%u delta=%.6f mode=%u "
              "params=%u",
              unsigned(effect.kind), unsigned(effect.soul_role), attribute, delta,
              unsigned(effect.value[8]), unsigned(effect.value[32]));
        }
      }
      log("COMBAT EFFECTS native committed source=%u target=%u count=%zu sent=%u", impact->source,
          impact->target, effects.size(), unsigned(sent));
    }
    log("COMBAT MELEE committed source=%u target=%u sent=%u native_result=%u", impact->source,
        impact->target, unsigned(sent), unsigned(result));
  }
  return result;
}
void* __fastcall hooked_destroy_rpg_event(void* self, char flags) {
  bool preview;
  {
    std::lock_guard<std::mutex> lock(pending_melee_mutex);
    preview = pending_melee.destroy(reinterpret_cast<std::uintptr_t>(self));
  }
  if (preview)
    log("COMBAT MELEE preview discarded; native event was never submitted");
  return original_destroy_rpg_event(self, flags);
}

std::optional<integration::CombatEquipmentTokens> committed_equipment_tokens(
    const integration::NativeMeleeData& cause, void* source_actor, void* target_actor) {
  integration::CombatEquipmentTokens tokens{};
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const auto item = integration::melee_u64(cause, integration::kCombatEquipmentOffsets[index]);
    const auto token =
        kcds::runtime::weapons::item_token(index == 2 ? target_actor : source_actor, item);
    if (!token) {
      return std::nullopt;
    }
    tokens[index] = *token;
  }
  return tokens;
}

bool resolve_committed_equipment(integration::NativeMeleeData& cause, void* source_actor,
                                 void* target_actor) {
  for (std::size_t index = 0; index < integration::kCombatEquipmentOffsets.size(); ++index) {
    const unsigned offset = integration::kCombatEquipmentOffsets[index];
    const auto token = integration::melee_u64(cause, offset);
    if (token > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    const auto item = kcds::runtime::weapons::item_from_token(
        index == 2 ? target_actor : source_actor, std::uint32_t(token));
    if (!item) {
      return false;
    }
    integration::melee_set_u64(cause, offset, *item);
  }
  return true;
}

void* __fastcall hooked_melee_hit(void* self, void** event, const void* data) {
  integration::NativeMeleeData cause{};
  core::EntityId target_id = core::kInvalidEntity;
  bool target_executor = false;
  void* source_movement = nullptr;
  auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  if (!replaying_melee && actor && data && local_network_id.load()) {
    auto soul = *reinterpret_cast<std::uint8_t**>(actor + 1640);
    std::memcpy(cause.data(), data, cause.size());
    const auto source = integration::melee_u64(cause, 0),
               target = integration::melee_u64(cause, 16);
    if (soul && target == *reinterpret_cast<std::uint64_t*>(soul + 48)) {
      auto executor = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + 120);
      std::lock_guard<std::mutex> lock(combat_identity_mutex);
      for (const auto& [remote, id] : remote_combat_souls) {
        if (source != *reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(remote) + 48))
          continue;
        if (executor == soul || executor == remote) {
          target_id = id;
          target_executor = executor == soul;
          const auto movement = remote_combat_movements.find(remote);
          if (movement != remote_combat_movements.end()) {
            source_movement = movement->second;
          }
        }
        break;
      }
      char dump[289]{};
      for (unsigned i = 0; i < 144; ++i)
        sprintf_s(dump + i * 2, sizeof(dump) - i * 2, "%02x", unsigned(cause[i]));
      log("COMBAT MELEE native source=%llx target=%llx self=%p target_executor=%u caller=%p "
          "data=%s",
          source, target, self, unsigned(target_executor), _ReturnAddress(), dump);
    }
  }
  auto result = original_melee_hit(self, event, data);
  if (target_id && event && *event) {
    auto source_actor =
        source_movement
            ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(source_movement) + 504)
            : nullptr;
    const auto equipment = committed_equipment_tokens(cause, source_actor, actor);
    if (!equipment) {
      log("COMBAT EFFECTS equipment identity unsupported source=%u target=%u", target_id,
          local_network_id.load());
      return result;
    }
    integration::MeleeEvent impact;
    impact.revision = requested_revision;
    impact.source = target_id;
    impact.target = local_network_id.load();
    impact.authority_is_target = true;
    impact.executor_is_target = target_executor;
    impact.data = integration::portable_committed_melee_data(cause, *equipment);
    // C_CombatSoul v42 also returns preview events. Only native EventManager
    // Submit commits them. The native destructor drops previews and prevents
    // a recycled allocation address from retaining a previous hit.
    std::lock_guard<std::mutex> lock(pending_melee_mutex);
    pending_melee.stage(reinterpret_cast<std::uintptr_t>(*event), impact);
  }
  return result;
}

// MCP dispatches the native source and victim movement signals.
// Victim listener queues the original CombatAction hit reaction;
// unlike the RPG cause, this does not subtract health again.
core::EntityId remote_combat_id(void* movement);
HitReaction original_hit_reaction;
thread_local void* observed_reaction_movement = nullptr;
std::intptr_t __fastcall hooked_hit_reaction(void* source, const void* info, void* target,
                                             const void* outcome) {
  integration::NativeReactionData data{};
  core::EntityId target_id = 0;
  auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  if (!replaying_melee && actor && source && target && info && outcome) {
    auto source_actor = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(source) + 504);
    auto victim_actor = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(target) + 504);
    if (source_actor == actor && remote_combat_id(target))
      return 0;
    if (victim_actor == actor)
      target_id = remote_combat_id(source);
    if (target_id) {
      std::memcpy(data.data(), info, 160);
      std::memcpy(data.data() + 160, outcome, 13);
      std::uint32_t weapon = 0;
      std::memcpy(&weapon, data.data() + 72, 4);
      if (weapon && weapon != *reinterpret_cast<std::uint32_t*>(source_actor + 48)) {
        target_id = 0;
      } else {
        data = integration::portable_reaction_data(data);
      }
    }
  }
  auto previous_observed = observed_reaction_movement;
  observed_reaction_movement = target;
  auto result = original_hit_reaction(source, info, target, outcome);
  observed_reaction_movement = previous_observed;
  if (target_id) {
    bool sent;
    {
      std::lock_guard<std::mutex> lock(network_mutex);
      sent = client->peer().send_reaction(target_id, local_network_id.load(), data, true);
    }
    log("COMBAT REACTION queue source=%u target=%u sent=%u", local_network_id.load(), target_id,
        unsigned(sent));
  }
  return result;
}
// Original remote animation events may reach the native impact producer.
// The victim client evaluates native defense/damage against its current input.
// The attacker replays that native result on the victim replica. The normal false
// return is handled by both native callers before any event is allocated.
unsigned native_action_type(void* movement);
bool paired_action(unsigned kind);
ProduceMelee original_produce_melee;
ProduceNativeHit produce_native_hit = nullptr;
CompletePhysicalAttackPower complete_physical_attack_power = nullptr;
SetNativeContactTarget set_native_contact_target = nullptr;

// Both original contact callers record whether
// the actual contacted movement is the selected opponent before evaluating the
// hit. The direct owner-evaluation path must retain this prelude as well as the
// power postamble; perfect block reads this property after native FailedAttack.
void prepare_native_contact_target(void* expected_core, void* contacted_movement) {
  auto native_core = static_cast<std::uint8_t*>(expected_core);
  if (!native_core || !set_native_contact_target) {
    return;
  }
  const auto selected = *reinterpret_cast<void**>(native_core + 3320);
  const bool same_target = contacted_movement && (!selected || selected == contacted_movement);
  set_native_contact_target(native_core + 1152, same_target ? 1 : 0);
}

// physical-context postamble: retain the original
// producer's power/flag writeback after a contact routed around that wrapper.
// The flag is the producer's own output, not the defence outcome or a network bool.
void finish_native_physical_contact(void* movement, void* expected_core, float power,
                                    char native_flag) {
  auto core = movement
                  ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528)
                  : nullptr;
  if (!core || core != expected_core || !complete_physical_attack_power || !std::isfinite(power)) {
    return;
  }
  const float native_value = native_flag ? 0.0f : power;
  complete_physical_attack_power(core + 480, native_value);
  log("COMBAT CONTACT physical power completion source=%u power=%.6f native_flag=%u value=%.6f",
      remote_combat_id(movement), power, unsigned(static_cast<unsigned char>(native_flag)),
      native_value);
}
const unsigned* native_backoff_attack_type = nullptr;
std::uint64_t local_attack_generation = 0;
std::unordered_map<core::EntityId, std::uint64_t> contacted_attack_generation;
// The swing that ran when the server cancelled the fight with a player (Player.stopFight, issue).
// The victim's game takes a blow from the contact the attacker's game relays, and the
// attacker's game detects it late in the swing: live, 330 ms after the cancel, with the new combat
// epoch, so the victim's stale-context fence let it land. The swing's generation is kept here and
// its contact on that player is not relayed; the next swing is a new generation and lands.
std::unordered_map<core::EntityId, std::uint64_t> retired_attack_generation;
// The local swing the generation counts: the game makes an attack at the button's press and again
// at its release for one swing, the same action both times (one swing hit twice, 2 x 16.4).
void* local_attack_action = nullptr;
ULONGLONG local_attack_started = 0;
// A swing's generation begins at the button's press, its attack at the game's Start (on release).
// In between, the previous swing's blade may still touch the player it hit: the game reports that
// contact again, and it went out as the new swing's hit (two hits 125-170 ms apart in one
// swing while attacks were spammed). The previous swing's victims are kept until the
// new attack starts, and a contact on one of them meanwhile is not relayed.
std::unordered_set<core::EntityId> previous_swing_victims;
bool local_swing_started = true;
void advance_local_contact_generation() {
  ++local_attack_generation;
  previous_swing_victims.clear();
  for (const auto& entry : contacted_attack_generation) {
    previous_swing_victims.insert(entry.first);
  }
  contacted_attack_generation.clear();
  retired_attack_generation.clear();
}
void local_swing_start(void* action) {
  if (action && action == local_attack_action && !local_swing_started) {
    local_swing_started = true;
    previous_swing_victims.clear();
  }
}
void retire_local_swing(core::EntityId opponent) {
  retired_attack_generation[opponent] = local_attack_generation;
  log("COMBAT cancel with %u: swing generation=%llu retired", opponent, local_attack_generation);
}
void begin_local_combat_contact(void* action) {
  advance_local_contact_generation();
  local_attack_action = action;
  local_attack_started = GetTickCount64();
  log("COMBAT native contact lifecycle generation=%llu action=%p", local_attack_generation, action);
  const auto kind = action ? slot<unsigned(__fastcall*)(void*)>(action, 14)(action) : 28;
  // Only an attack waits for its Start; any other action is under way already.
  local_swing_started = kind != 3 && kind != 4;
  if (kind != 3 && kind != 4) {
    return;
  }
  std::vector<core::EntityId> recipients;
  {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    for (const auto& entry : remote_combat_souls) {
      recipients.push_back(entry.second);
    }
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  for (const auto recipient : recipients) {
    client->peer().send_attack_generation(local_network_id.load(), recipient,
                                          local_attack_generation, std::uint8_t(kind));
  }
}

MasterRiposteStart original_master_riposte_start = nullptr;

std::intptr_t __fastcall hooked_master_riposte_start(void* action) {
  if (action && GetCurrentThreadId() == engine_update_thread.load()) {
    auto native_action = static_cast<std::uint8_t*>(action);
    auto movement = *reinterpret_cast<std::uint8_t**>(native_action + 136);
    auto actor = movement ? *reinterpret_cast<void**>(movement + 504) : nullptr;
    // IDA181821F5C starts the original master-strike counter: phase216
    // changes2->3 before its attack helper starts. The preceding deflection
    // already emitted a contact from this same parent action. Give this new
    // native strike its own deduplication generation before a synchronous hit.
    if (actor == local_combat_actor.load() && actor &&
        slot<unsigned(__fastcall*)(void*)>(action, 14)(action) == 9 &&
        *reinterpret_cast<unsigned*>(native_action + 216) == 2) {
      advance_local_contact_generation();
      // Preserve the factory's press/release identity: an unused normal attack
      // returned during this pair must not count again at the button's release.
      log("COMBAT master counter lifecycle generation=%llu action=%p", local_attack_generation,
          action);
    }
  }
  return original_master_riposte_start(action);
}
std::unordered_map<core::EntityId, kcds::runtime::combat::ReplicaContact> replica_contacts;

std::optional<kcds::runtime::combat::ReplicaContactScope> replica_contact_scope(
    core::EntityId player, void* actor, void* movement) {
  if (!player || !actor || !movement || actor == local_combat_actor.load() ||
      GetCurrentThreadId() != engine_update_thread.load()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  const auto epoch = client->peer().combat_epoch(player);
  if (!epoch) {
    return std::nullopt;
  }
  return kcds::runtime::combat::ReplicaContactScope{requested_revision, *epoch,
                                                    reinterpret_cast<std::uintptr_t>(actor),
                                                    reinterpret_cast<std::uintptr_t>(movement)};
}
// The last attack a replica started here (its owner's replayed press): who and when.
core::EntityId remote_attack_source = 0;
ULONGLONG remote_attack_started = 0;
// Lets the game animate a player's replica itself for a moment (its block or parry).
void open_reaction_window(core::EntityId player);
// The local attacker's swing held at the contact until the victim's owner answers: its
// layers' playback scales (C_SkeletonAnim::SetLayerPlaybackScale: the float at
// + 80 + 64 * layer) are set to 0 and put back with the answer, so a parry decided there shows
// the blades meeting once instead of the swing passing through and a second clash later.
SwingHold swing_hold;
// The longest a swing waits for the victim's answer, in ms.
constexpr ULONGLONG kSwingHoldMs = 300;
constexpr std::size_t kLayerPlaybackScale = 80;
constexpr std::size_t kLayerStride = 64;

float& layer_playback_scale(void* skeleton, unsigned layer) {
  return *reinterpret_cast<float*>(static_cast<std::uint8_t*>(skeleton) + kLayerPlaybackScale +
                                   kLayerStride * layer);
}

void hold_local_swing(core::EntityId victim) {
  void* skeleton = local_skeleton.load();
  auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto movement = actor ? *reinterpret_cast<void**>(actor + 632) : nullptr;
  if (!skeleton || swing_hold.skeleton || paired_action(native_action_type(movement))) {
    return;
  }
  swing_hold.skeleton = skeleton;
  swing_hold.victim = victim;
  swing_hold.started = GetTickCount64();
  swing_hold.until = swing_hold.started + kSwingHoldMs;
  for (unsigned layer = 0; layer < 16; ++layer) {
    swing_hold.scales[layer] = layer_playback_scale(skeleton, layer);
    layer_playback_scale(skeleton, layer) = 0.0f;
  }
  log("COMBAT SWING held at the contact with player=%u", victim);
}

// Puts the held swing back: on the victim's answer or deflection (answered) or when the hold runs
// out.
void release_local_swing(bool answered) {
  if (!swing_hold.skeleton) {
    return;
  }
  if (swing_hold.skeleton == local_skeleton.load()) {
    for (unsigned layer = 0; layer < 16; ++layer) {
      layer_playback_scale(swing_hold.skeleton, layer) = swing_hold.scales[layer];
    }
  }
  log("COMBAT SWING released (%s) after %lld ms", answered ? "answer" : "timeout",
      static_cast<long long>(GetTickCount64() - swing_hold.started));
  swing_hold = SwingHold{};
}

// A defended swing goes on with the game's deflection clip: the next clip it starts on the local
// fighter's base layer.
void local_clip_started(void* skeleton, int id, unsigned layer) {
  if (!swing_hold.awaiting_deflection || skeleton != swing_hold.skeleton || layer != 0) {
    return;
  }
  log("COMBAT SWING deflection clip=%d", id);
  release_local_swing(true);
}

// Remote players whose replicated uiBlocking field is set (guarded by combat_identity_mutex).
std::unordered_set<core::EntityId> remote_ui_blocking;
// Last time (GetTickCount64) the local player had a full-screen UI open; 0 when never.
std::atomic<std::uint64_t> local_ui_blocking_ms{0};

char relay_owned_native_contact(void* self, void* info, float* native_power, char* native_flag,
                                const void* context, bool commit, unsigned native_state,
                                void* outcome) {
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(self) + 8);
  auto actor = movement ? *reinterpret_cast<std::uint8_t**>(movement + 504) : nullptr;
  auto victim_move =
      context
          ? *reinterpret_cast<std::uint8_t* const*>(static_cast<const std::uint8_t*>(context) + 16)
          : nullptr;
  auto victim_id = remote_combat_id(victim_move);
  // D568B0 also resolves a physical target through AttackParams+36 when
  // context+16 is empty. Mirror that identity route; collision stays native.
  if (!victim_id && context) {
    auto attack = *reinterpret_cast<const std::uint8_t* const*>(
        static_cast<const std::uint8_t*>(context) + 24);
    const auto target_engine = attack ? *reinterpret_cast<const unsigned*>(attack + 36) : 0;
    if (target_engine) {
      std::lock_guard<std::mutex> lock(combat_identity_mutex);
      for (const auto& entry : remote_combat_movements) {
        auto candidate =
            *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(entry.second) + 504);
        if (candidate && *reinterpret_cast<const unsigned*>(candidate + 48) == target_engine) {
          auto identity = remote_combat_souls.find(entry.first);
          if (identity != remote_combat_souls.end())
            victim_id = identity->second;
          break;
        }
      }
    }
  }
  if (commit && actor == local_combat_actor.load() && victim_id &&
      native_action_type(movement) == kNativeMasterStruckAction &&
      native_action_type(victim_move) == kNativeMasterStrikeAction) {
    // The local fighter is master-struck and its blade meets the striker inside their pair. Both
    // halves started here on one frame, so the striker's replica is in the pair phase a game alone
    // would have at this contact; on the striker's own screen the contact arrives later, after its
    // deflecting phase (20.34 damage to the striker, whose counter was then cut off). The
    // game's own preview decides here; only a hit it does not deflect goes to the striker.
    std::array<std::uint8_t, 160> saved_info{};
    std::array<std::uint8_t, 13> saved_outcome{};
    std::memcpy(saved_info.data(), info, saved_info.size());
    std::memcpy(saved_outcome.data(), outcome, saved_outcome.size());
    const float saved_power = *native_power;
    const char saved_flag = *native_flag;
    const char produced = produce_native_hit(self, info, native_power, native_flag, context, false,
                                             native_state, outcome);
    const auto* verdict = static_cast<const std::uint8_t*>(outcome);
    const bool deflected = !produced || verdict[1] || verdict[3];
    log("COMBAT CONTACT master-struck blade target=%u produced=%u defence=%u/%u deflected=%u",
        victim_id, unsigned(produced), unsigned(verdict[1]), unsigned(verdict[3]),
        unsigned(deflected));
    std::memcpy(info, saved_info.data(), saved_info.size());
    std::memcpy(outcome, saved_outcome.data(), saved_outcome.size());
    *native_power = saved_power;
    *native_flag = saved_flag;
    if (deflected) {
      return 0;
    }
  }
  if (commit && actor == local_combat_actor.load() && victim_id) {
    bool contact_sent_now = false;
    const auto retired = retired_attack_generation.find(victim_id);
    if (retired != retired_attack_generation.end() && retired->second == local_attack_generation) {
      // Started before Player.stopFight parted the pair: this swing's contact stays here, once
      // (the swing's later contacts on the victim are the same generation's duplicates).
      log("COMBAT CONTACT retired source=%u target=%u generation=%llu: the swing started before "
          "the cancel",
          local_network_id.load(), victim_id, local_attack_generation);
      retired_attack_generation.erase(retired);
      contacted_attack_generation[victim_id] = local_attack_generation;
    } else if (!local_swing_started && previous_swing_victims.count(victim_id)) {
      log("COMBAT CONTACT previous swing's blade on %u not relayed: generation=%llu has not "
          "started",
          victim_id, local_attack_generation);
    } else if (!contacted_attack_generation.count(victim_id) ||
               contacted_attack_generation.at(victim_id) != local_attack_generation) {
      integration::NativeContactData data{};
      integration::NativeReactionData hit{};
      std::memcpy(hit.data(), info, 160);
      hit = integration::portable_reaction_data(hit);
      std::copy_n(hit.begin(), 160, data.begin());
      auto attack = *reinterpret_cast<const std::uint8_t* const*>(
          static_cast<const std::uint8_t*>(context) + 24);
      std::memcpy(data.data() + 160, attack, 144);
      std::fill_n(data.begin() + 160, 12, std::uint8_t(0));
      std::fill_n(data.begin() + 196, 4, std::uint8_t(0));
      std::fill_n(data.begin() + 296, 8, std::uint8_t(0));
      std::memcpy(data.data() + 304, context, 16);
      auto begin = *reinterpret_cast<const std::uint8_t* const*>(
          static_cast<const std::uint8_t*>(context) + 32);
      auto end = *reinterpret_cast<const std::uint8_t* const*>(
          static_cast<const std::uint8_t*>(context) + 40);
      const float power = *native_power;
      const auto state = native_state;
      std::memcpy(data.data() + 449, &power, 4);
      std::memcpy(data.data() + 453, &state, 4);
      std::memcpy(data.data() + 457, outcome, 13);
      if (end >= begin && end - begin <= 128 && (end - begin) % 16 == 0) {
        data[320] = std::uint8_t((end - begin) / 16);
        if (data[320])
          std::memcpy(data.data() + 321, begin, end - begin);
        bool sent;
        {
          std::lock_guard<std::mutex> lock(network_mutex);
          sent = client->peer().send_native_contact(local_network_id.load(), victim_id, data,
                                                    local_attack_generation);
        }
        if (sent) {
          contacted_attack_generation[victim_id] = local_attack_generation;
          contact_sent_now = true;
        }
        log("COMBAT CONTACT source=%u target=%u generation=%llu state=%u modifiers=%u sent=%u "
            "victim_action=%u",
            local_network_id.load(), victim_id, local_attack_generation, state, unsigned(data[320]),
            unsigned(sent), native_action_type(victim_move));
      }
    } else if (native_action_type(movement) == 9) {
      log("COMBAT CONTACT duplicate master strike suppressed target=%u generation=%llu", victim_id,
          local_attack_generation);
    }
    // A victim whose replica blocks or parries right now (its owner's block input is replayed on
    // it): the game evaluates the contact here as well, so the blades meet and the attacker
    // recoils at the contact, as in a game alone (waiting for the owner's reaction,
    // the attack ran on 440 ms before the recoil). This preview does not dispatch the native
    // source/victim movement signals: hooked_hit_reaction suppresses that local-source call.
    // The owner's authoritative reaction must still dispatch those signals on receipt.
    const unsigned victim_action = native_action_type(victim_move);
    if (victim_action == kNativeGuardAction || victim_action == kNativeDefenceAction) {
      // A guard's block reaction is the game's own here, since its owner's clips come a round trip
      // late. A perfect block already plays its owner's own clips (relayed): the game's
      // return to guard would follow its own parry variant instead, and the sword jumped from the
      // owner's low end pose to that variant's high one (live video, parry 3).
      if (victim_action == kNativeGuardAction) {
        open_reaction_window(victim_id);
      }
      log("COMBAT CONTACT predicted defence target=%u", victim_id);
      // The native preview (commit=false) computes HitInfo and the defence outcome
      // without submitting RPG effects or equipment wear. The victim's owner commits the hit.
      const char produced = produce_native_hit(self, info, native_power, native_flag, context,
                                               false, native_state, outcome);
      // Defended here (the outcome's defence bytes +1 and +3, as in the owner's reaction): the
      // swing waits at the blades for its deflection, which then comes at once.
      const auto* verdict = static_cast<const std::uint8_t*>(outcome);
      if (contact_sent_now && (verdict[1] || verdict[3])) {
        hold_local_swing(victim_id);
        swing_hold.until = GetTickCount64() + kDeflectionWaitMs;
        swing_hold.awaiting_deflection = true;
      }
      return produced;
    }
    // Not defending here (yet): the swing waits at the contact for the owner's answer.
    if (contact_sent_now) {
      hold_local_swing(victim_id);
    }
    // Only the victim owner may commit damage or select victim reactions.
    // Returning false stops both native callers before predictive pair actions.
    return 0;
  }
  return produce_native_hit(self, info, native_power, native_flag, context, commit, native_state,
                            outcome);
}
// The weapon contact callback of an attacker (registered per attacker in the
// list at movement+536 +40 as {listener, callback}): from the physical contact's attack record
// (+200 the victim's movement) it evaluates the hit, decides whether the reaction is
// already handled (the victim keeps its lock) and queues the reaction and hit animation.
// A relayed physical contact runs it on the victim's owner as the replica's own collision would
// so the lock and the hit animation are the game's own.
PhysicalContact native_physical_contact = nullptr;

// The combat module's own hit reaction. The game's hit callers (for an
// attack, for a physical contact) create a C_CombatActorActionHit for a victim in
// combat (only its +8, the source movement, is read from the caller), push it to the
// victim's action driver and mark the reaction handled (flag 0x2 cleared, +117), so
// GameRules plays no generic reaction. Without it the victim leaves its combat actions for the
// generic reaction and the lock marker blinks until the guard returns.
CreateHitAction create_hit_action = nullptr;
PushCombatAction push_combat_action = nullptr;
HitActionVariant hit_action_variant = nullptr;
ActionHandlesHit action_handles_hit = nullptr;
// HitInfo+68: the hit strength the variant is chosen from.
constexpr std::size_t kHitStrength = 68;
// Stance bit (core+2976) for which the standard hit variant is not used.
constexpr unsigned kNonStandardHitStance = 0x10000;

// Starts the victim's combat hit action as the game's hit callers do; true when it was pushed.
bool start_native_hit_action(void* source, void* victim, const std::uint8_t* hit, unsigned stance) {
  if (!create_hit_action || !push_combat_action || !hit_action_variant) {
    return false;
  }
  unsigned strength = 0;
  std::memcpy(&strength, hit + kHitStrength, 4);
  const char standard = (stance & kNonStandardHitStance) ? 0 : 1;
  const int variant = hit_action_variant(nullptr, strength, standard);
  const std::array<void*, 2> caller{nullptr, source};
  void* action = nullptr;
  create_hit_action(caller.data(), &action, victim, variant, hit);
  if (!action) {
    return false;
  }
  const bool pushed = push_combat_action(victim, &action) != 0;
  slot<void(__fastcall*)(void*)>(action, 2)(action);
  return pushed;
}
// A player whose own game started its hit action for another player's blow: its HitAction
// marker and its reaction (its game's evaluated hit, with this client's engine ids; the
// attacker's client and every watcher get it) meet here in either order, and its replica gets the
// same native hit action, with the attacker here (the local player or its replica) as the source.
// The push ends the replica's running attack as the owner's game ended its own (the replica
// swung on through its owner's hit reaction). Engine thread.
std::unordered_map<core::EntityId, ReplicaHit> replica_hits;
// How far apart the marker and the reaction may come (they travel on different streams).
constexpr ULONGLONG kReplicaHitPairMs = 600;
// `victim_move`: the victim's replica movement now; it must still be the one the reaction found.
void give_replica_hit_action(core::EntityId victim, void* victim_move) {
  const auto found = replica_hits.find(victim);
  if (found == replica_hits.end()) {
    return;
  }
  // A half older than the pairing window belongs to an earlier blow (a marker whose
  // reaction never came here was paired with the next blow's reaction 25 s later).
  ReplicaHit& pending = found->second;
  const ULONGLONG now = GetTickCount64();
  if (pending.marked && now - pending.marked > kReplicaHitPairMs) {
    pending.marked = 0;
  }
  if (pending.reacted && now - pending.reacted > kReplicaHitPairMs) {
    pending.reacted = 0;
  }
  if (!pending.marked && !pending.reacted) {
    replica_hits.erase(found);
    return;
  }
  if (!pending.marked || !pending.reacted) {
    return;
  }
  const ReplicaHit hit = pending;
  replica_hits.erase(found);
  void* source_move = hit.source_move;
  auto source_core =
      source_move ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(source_move) + 528)
                  : nullptr;
  if (!source_core || !victim_move || victim_move != hit.victim_move) {
    log("COMBAT replica hit action not given player=%u: its movements changed", victim);
    return;
  }
  const unsigned stance =
      slot<unsigned(__fastcall*)(void*)>(source_core + 2976, 1)(source_core + 2976);
  const unsigned before = native_action_type(victim_move);
  auto data = hit.data;
  const bool started = start_native_hit_action(source_move, victim_move, data.data(), stance);
  log("COMBAT replica hit action as by its owner's game player=%u started=%u action=%u->%u", victim,
      unsigned(started), before, native_action_type(victim_move));
}
thread_local bool replaying_physical_contact = false;
std::unordered_map<void*, void*> physical_contact_listeners;

thread_local bool physical_contact_evaluated = false;

// The game registers a replica's weapon contact listener only while its attack runs
// (adds it, removes it), and the replica's swing runs behind the relayed
// contact; the listener object itself lasts with the replica, so the last one seen is kept per
// replica movement (cleared when replicas are removed). Engine thread only.

// The replica attacker's weapon contact listener; nullptr when it was never seen.
// Whether a listener can take a replayed contact: its callback reads the
// listener's movement and that movement's combat parts (+528, +944) unchecked. After a knock-out
// and revive a remembered listener had lost them; the replay faulted and the engine adapter
// stopped.
bool usable_contact_listener(const std::uint8_t* listener, const void* movement) {
  auto owner = listener ? *reinterpret_cast<std::uint8_t* const*>(listener + 8) : nullptr;
  return owner == movement && *reinterpret_cast<void* const*>(owner + 528) &&
         *reinterpret_cast<void* const*>(owner + 944);
}

void* physical_contact_listener(void* movement) {
  auto signals = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 536);
  auto entries = signals ? *reinterpret_cast<std::uint8_t**>(signals + 40) : nullptr;
  if (entries && native_physical_contact) {
    const int count = *reinterpret_cast<const int*>(entries - 4);
    for (int index = 0; index < count && index < 64; ++index) {
      auto entry = entries + index * 16;
      if (*reinterpret_cast<void**>(entry + 8) ==
          reinterpret_cast<void*>(native_physical_contact)) {
        physical_contact_listeners[movement] = *reinterpret_cast<void**>(entry);
        break;
      }
    }
  }
  auto known = physical_contact_listeners.find(movement);
  if (known == physical_contact_listeners.end()) {
    return nullptr;
  }
  if (!usable_contact_listener(static_cast<std::uint8_t*>(known->second), movement)) {
    physical_contact_listeners.erase(known);
    return nullptr;
  }
  return known->second;
}

// Horses (riding/native_horses): whether an actor is a horse this client simulates, or a replica of
// a horse another client simulates.
bool simulated_horse_actor(void* actor);
bool horse_replica_actor(void* actor);
char __fastcall hooked_produce_melee(void* self, void* info, const void* context, void* outcome) {
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(self) + 8);
  auto actor = movement ? *reinterpret_cast<void**>(movement + 504) : nullptr;
  auto victim =
      context
          ? *reinterpret_cast<std::uint8_t* const*>(static_cast<const std::uint8_t*>(context) + 16)
          : nullptr;
  // Preserve the original AttackParams entity fallback even if the physical
  // callback omitted its movement pointer. Resolve only registered humans.
  if (!victim && context) {
    auto attack = *reinterpret_cast<const std::uint8_t* const*>(
        static_cast<const std::uint8_t*>(context) + 24);
    const auto engine_id = attack ? *reinterpret_cast<const unsigned*>(attack + 36) : 0;
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    for (const auto& entry : remote_combat_movements) {
      auto candidate =
          *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(entry.second) + 504);
      if (engine_id && candidate && *reinterpret_cast<unsigned*>(candidate + 48) == engine_id) {
        victim = static_cast<std::uint8_t*>(entry.second);
        break;
      }
    }
  }
  auto core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
  if (!core)
    return original_produce_melee(self, info, context, outcome);
  // D53610 native backoff has no RPG impact and returns true so its
  // original caller can finish the F push pair on both sides.
  if (slot<unsigned(__fastcall*)(void*)>(core + 528, 1)(core + 528) == *native_backoff_attack_type)
    return original_produce_melee(self, info, context, outcome);
  const bool synchronized = context && *static_cast<const std::uint8_t*>(context);
  const auto remote = remote_combat_id(movement);
  if (remote && replaying_physical_contact && !synchronized) {
    // The relayed contact replayed through the replica's own weapon contact callback.
    physical_contact_evaluated = true;
    return original_produce_melee(self, info, context, outcome);
  }
  if (remote) {
    // A replica's swing reaches this client 50-80 ms late and its collision
    // with the local player lands late in the swing (about 500 ms
    // after the attacker's own contact). The attacker's owner relays every
    // contact through KCM7 instead, so the replica's collision is ignored; the
    // local player still evaluates the relayed contact against its live defense.
    // A horse is hurt on the client that simulates it, like a player on its own client (a
    // horse took no damage from another player's weapon).
    auto horse_actor = victim ? *reinterpret_cast<void**>(victim + 504) : nullptr;
    if (horse_actor && simulated_horse_actor(horse_actor)) {
      log("COMBAT horse hit source=%u", remote);
      return original_produce_melee(self, info, context, outcome);
    }
    return 0;
  }
  // The local player's weapon on another client's horse: that client hurts the horse.
  auto victim_horse = victim ? *reinterpret_cast<void**>(victim + 504) : nullptr;
  if (actor == local_combat_actor.load() && victim_horse && horse_replica_actor(victim_horse))
    return 0;
  if (actor == local_combat_actor.load() && remote_combat_id(victim)) {
    if (synchronized) {
      float power = slot<float(__fastcall*)(void*)>(core + 480, 1)(core + 480);
      const auto state = slot<unsigned(__fastcall*)(void*)>(core + 528, 1)(core + 528);
      char flag = 0;
      return relay_owned_native_contact(self, info, &power, &flag, context, true, state, outcome);
    }
    // Physical contacts are detected here, when the attacker's own swing
    // touches the replica, and relayed through the owner contact path (KCM7).
    // The victim's owner evaluates them with the original native callback
    // against its live defense, so blocks and damage stay its decision; only
    // the moment of contact follows the attacker's swing.
    float power = slot<float(__fastcall*)(void*)>(core + 480, 1)(core + 480);
    const auto state = slot<unsigned(__fastcall*)(void*)>(core + 528, 1)(core + 528);
    char flag = 0;
    const auto produced =
        relay_owned_native_contact(self, info, &power, &flag, context, true, state, outcome);
    finish_native_physical_contact(movement, core, power, flag);
    return produced;
  }
  return original_produce_melee(self, info, context, outcome);
}

void tick_swing_hold() {
  // A swing held at a contact goes on once its victim took too long to answer.
  if (swing_hold.skeleton && GetTickCount64() >= swing_hold.until) {
    release_local_swing(false);
  }
  if (swing_hold.skeleton) {
    auto actor = static_cast<std::uint8_t*>(local_combat_actor.load());
    auto movement = actor ? *reinterpret_cast<void**>(actor + 632) : nullptr;
    if (paired_action(native_action_type(movement))) {
      release_local_swing(true);
    }
  }
}

}  // namespace kcds::game_client
