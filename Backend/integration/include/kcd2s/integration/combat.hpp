#pragma once
#include <array>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include "kcd2s/core/ids.hpp"

namespace kcd2s::integration {
// Separate reliable ordered channel; never coalesce impacts into a latest-value snapshot.
inline constexpr std::uint8_t kCombatChannel = 6;
// Optional spectator previews must not consume an unbounded authority queue.
inline constexpr std::size_t kMaxObserverContacts = 64;
inline constexpr unsigned kMaxObserverContactsPerTick = 4;
inline constexpr std::size_t kObserverReliableLimit = 128;
// WHGame 1.5.5 S_CombatHitData value cause, not a pointer-bearing HitInfo/Actor packet.
// Soul IDs are removed before sending and resolved from network identities on the receiver.
// CryEntity IDs at +8/+24 and native padding are also removed.
// Item instance IDs require a separate equipment identity mapping; currently only unarmed
// causes with native empty item IDs are supported. No health or invented damage amount.
using NativeMeleeData = std::array<std::uint8_t, 144>;
inline std::uint64_t melee_u64(const NativeMeleeData& d, std::size_t at) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i)
    v |= std::uint64_t(d[at + i]) << (8 * i);
  return v;
}
inline void melee_set_u64(NativeMeleeData& d, std::size_t at, std::uint64_t v) {
  for (unsigned i = 0; i < 8; ++i)
    d[at + i] = std::uint8_t(v >> (8 * i));
}
inline bool valid_melee_data(const NativeMeleeData& d) {
  if (melee_u64(d, 0) || melee_u64(d, 8) || melee_u64(d, 16) || melee_u64(d, 24) || d[102] ||
      d[103])
    return false;
  for (unsigned at : {120u, 128u, 136u})
    if (melee_u64(d, at) != 0)
      return false;
  // Numeric coefficients/vectors consumed by the native processor; reject NaN/Inf.
  for (unsigned at : {44u, 48u, 52u, 56u, 60u, 64u, 68u, 72u, 108u, 112u}) {
    float value;
    std::memcpy(&value, d.data() + at, 4);
    if (!std::isfinite(value))
      return false;
  }
  return true;
}
inline NativeMeleeData portable_melee_data(const NativeMeleeData& native) {
  auto data = native;
  for (unsigned at : {0u, 8u, 16u, 24u})
    melee_set_u64(data, at, 0);
  data[102] = data[103] = 0;
  return data;
}
// Committed effects carry owner-generated equipment tokens, never native item instances. KCM8 keeps
// these semantics distinct from the older KCM6 unarmed-only cause. The first two tokens
// name the source's attack/secondary item; the third names the target's defence item.
using CombatEquipmentTokens = std::array<std::uint32_t, 3>;
inline constexpr std::array<unsigned, 3> kCombatEquipmentOffsets{120, 128, 136};

inline NativeMeleeData portable_committed_melee_data(const NativeMeleeData& native,
                                                     const CombatEquipmentTokens& tokens) {
  auto data = portable_melee_data(native);
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    melee_set_u64(data, kCombatEquipmentOffsets[index], tokens[index]);
  }
  return data;
}

inline bool valid_committed_melee_data(const NativeMeleeData& data) {
  auto unarmed = data;
  for (const unsigned offset : kCombatEquipmentOffsets) {
    if (melee_u64(data, offset) > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    melee_set_u64(unarmed, offset, 0);
  }
  return valid_melee_data(unarmed);
}
// Native S_HitInfo (160 bytes) followed by the 13-byte native hit outcome.
// Only scalar data; process-local entities/item instance and padding are fenced. The weapon's
// item instance (+8..+15) is the sender's own handle: it is cleared, so a block or parry against a
// weapon reaches the attacker too (only blows with fists showed their block).
// The receiving client puts in its own handle of the attacker's weapon (weapons/drawn_weapons.hpp
// striking_item): the game plays the blow's impact effect and sound by it.
using NativeReactionData = std::array<std::uint8_t, 173>;
inline NativeReactionData portable_reaction_data(NativeReactionData data) {
  for (unsigned at : {0u, 40u, 72u})
    std::fill_n(data.begin() + at, 4, std::uint8_t(0));
  std::fill_n(data.begin() + 8, 8, std::uint8_t(0));
  for (unsigned at : {4u, 5u, 6u, 7u, 37u, 38u, 39u, 66u, 67u, 119u, 156u, 157u, 158u, 159u})
    data[at] = 0;
  return data;
}
inline bool valid_reaction_data(const NativeReactionData& data) {
  if (portable_reaction_data(data) != data)
    return false;
  for (unsigned at :
       {44u, 48u, 80u, 84u, 88u, 92u, 96u, 100u, 104u, 108u, 112u, 140u, 144u, 148u, 152u}) {
    float value;
    std::memcpy(&value, data.data() + at, 4);
    if (!std::isfinite(value))
      return false;
  }
  return data[172] <= 1;
}
// Original SyncHit database identity: action definition, fragment and tag masks
// (+8..+63), native boolean/context, the source SyncAttack header (+8..+63),
// and source native action flags. KCM5 carries no process-local pointers.
using NativeSyncHitData = std::array<std::uint8_t, 121>;
inline bool valid_sync_hit_data(const NativeSyncHitData& d) {
  std::uint32_t context = 0;
  std::memcpy(&context, d.data() + 57, 4);
  return std::any_of(d.begin(), d.begin() + 56, [](auto b) { return b != 0; }) && d[56] <= 1 &&
         (context == 0 || context == 256) &&
         std::any_of(d.begin() + 61, d.begin() + 117, [](auto b) { return b != 0; });
}
// Replay native combat input, including its press/release timing. No block
// outcome, damage multiplier, or perfect-block time window is network-authored.
enum class CombatAction : std::uint8_t {
  Block = 1,
  Prepare,
  AttackMouse,
  Attack,
  Abort,
  Lock,
  Unlock,
  ZoneX,
  ZoneY,
  Target,
  MoveLeft,
  MoveRight,
  MoveForward,
  MoveBack,
  Special,
  // The attacker's native attack zone (value 0..15) sent just before an attack press or release,
  // so the replica's attack factory picks the same attack (for example the zone-5 kick) instead
  // of the zone it accumulated from relayed mouse deltas.
  AttackZone,
  // Clinch handshake between a player and the client of the other player. Mode 1 asks the other
  // client to start a clinch the sender initiates, mode 2 tells the initiator that the other client
  // started it, mode 4 that it refused it (answered at once, nothing waits). Mode 3 tells
  // every other client that the sender's own game started the clinch it initiates against the
  // player its lock is on, so a watcher's two replicas hold the same native clinch (its push and
  // its attacks need it there too). The value is the native clinch kind: 0 from a guard start, 1
  // from an approach.
  Clinch,
  // The sender's own game turned its block press into a perfect block. Its replicas start
  // the same native perfect block against their lock target; the value is the block's variant.
  PerfectBlock,
  // Sent just before a block press made while another player's attack runs: how long
  // after that attack started the sender pressed, in whole ms on the sender's screen. A receiver
  // whose own screen shows the press later than that brings the sender's guard up to time.
  DefenceTiming,
  // Whom the sender's combat lock is on, for the clients it is not on: the value is that
  // player's network id, 0 for none. Sent after Target mode 2, so a watcher's replica of the
  // sender locks onto the same player's replica and the native guard, parry and counter of a
  // duel between two other players play there as well.
  TargetOther,
  // The sender's own game turned its defence into the native SYNC_PERFECT_BLOCK, whose
  // swing deflects the attacker's and strikes back in one move: mode 1 the master strike (an
  // attack pressed into the other's swing), mode 2 a perfect block of a paired (sync) attack. Its
  // replicas start the same native one against their lock target. The value is the request of the
  // sender's start (for the master strike the input that made it, e.g. its attack press), 255 for
  // none: the game finds the pair's animation rows by it.
  SyncPerfectBlock,
  // Sent just before SyncPerfectBlock: the sender's guard (the native guard zone, 0..15)
  // at that start. The pair's rows need the defender's guard opposite the incoming attack,
  // and a replica's guard follows relayed mouse deltas, not the owner's guard
  // (guard 1 on the replica while its owner's master strike started from guard 2).
  DefenceGuard,
  // One of the sender's combat-model int properties, sent when it changes: mode 1 the
  // stance (core + 192), mode 2 the guard zone (core + 240), mode 3 the value at core + 96; the
  // value 0..255. The game reads all three of both fighters for a pair's rows,
  // and a replica's drifted from its owner's: a pair swaps both stances (every
  // master strike after the first was refused on the struck player's screen), the guard follows
  // relayed mouse deltas, and core + 96 came about 500 ms late on a watcher.
  CombatModel,
  // Sent just before SyncPerfectBlock with DefenceGuard: the sender's combat-model int
  // property at core + 96 (0..255) at that start, which the pair-row query reads for the
  // defender. Its game set it to 24 when the paired attack began on it; a
  // watcher's replica got it only after the owner's paired parry had been refused there.
  DefenceMode,
  // Sent just before an attack press or release (AttackMouse, Attack) that the sender's own game
  // made nothing of: no attack from its attack factory and no riposte (the press did not
  // fall into the open riposte window). The replica's game does not get that input either.
  // Without it a replica swung or riposted where its owner did not (parry and counter
  // loop: a press during the owner's recovery or its own parry).
  AttackRefused,
  // Sent just before an attack press or release that made the sender's own game ask for its
  // riposte (the riposte request, core + 912, which the game sets only while the riposte
  // window is open). The replica's game asks for the same riposte instead of taking
  // that input; an attack input without it never becomes a riposte there. The replica's window
  // opens at its own moment, and at the window's edge the two games decided differently (the
  // replica riposted where its owner's game took the press as an attack).
  Riposte,
  // Sent when the sender's own game started its combat hit action for another player's blow
  // The attacker's client starts the same native hit action on the sender's replica with
  // the sender's reaction (the game's own evaluated hit), so the replica's attack ends as its
  // owner's did (the replica swung on through the owner's hit reaction).
  HitAction,
  // The sender's own game made its dodge (the jump key with a direction in a locked fight,
  // the game's dodge request creating C_CombatActorActionDodge, action 8, combat
  // state 512). The value is the input direction the request dodged in, in degrees (atan2 of its
  // forward and right parts: 0 right, 90 forward, 180 left, -90 back). Its replicas start the same
  // native dodge through their own game's request in that direction; the dodge key itself is never
  // replayed, so a replica dodges only where its owner's game did.
  Dodge
};
// Network ids travel as whole floats; every id below 2^24 is exact.
constexpr float kMaxCombatTargetId = 16777215.0f;
// A dodge direction is an angle in degrees within a half turn either way.
constexpr float kMaxDodgeDegrees = 180.0f;
struct CombatActionData {
  CombatAction action{};
  std::uint8_t mode{};
  float value{};
};
inline bool valid_combat_action(const CombatActionData& a) {
  const auto id = unsigned(a.action);
  if (a.action == CombatAction::AttackZone)
    return a.mode == 1 && a.value >= 0 && a.value <= 15 && a.value == std::floor(a.value);
  if (a.action == CombatAction::Clinch)
    return a.mode >= 1 && a.mode <= 4 && (a.value == 0 || a.value == 1);
  if (a.action == CombatAction::PerfectBlock)
    return a.mode == 1 && a.value >= 0 && a.value <= 255 && a.value == std::floor(a.value);
  if (a.action == CombatAction::SyncPerfectBlock)
    return (a.mode == 1 || a.mode == 2) && a.value >= 0 && a.value <= 255 &&
           a.value == std::floor(a.value);
  if (a.action == CombatAction::DefenceGuard)
    return a.mode == 1 && a.value >= 0 && a.value <= 15 && a.value == std::floor(a.value);
  if (a.action == CombatAction::CombatModel)
    return a.mode >= 1 && a.mode <= 3 && a.value >= 0 && a.value <= 255 &&
           a.value == std::floor(a.value);
  if (a.action == CombatAction::DefenceMode)
    return a.mode == 1 && a.value >= 0 && a.value <= 255 && a.value == std::floor(a.value);
  if (a.action == CombatAction::AttackRefused || a.action == CombatAction::Riposte ||
      a.action == CombatAction::HitAction)
    return a.mode == 1 && a.value == 0;
  if (a.action == CombatAction::DefenceTiming)
    return a.mode == 1 && a.value >= 0 && a.value <= 5000 && a.value == std::floor(a.value);
  if (a.action == CombatAction::TargetOther)
    return a.mode == 1 && a.value >= 0 && a.value <= kMaxCombatTargetId &&
           a.value == std::floor(a.value);
  if (a.action == CombatAction::Dodge)
    return a.mode == 1 && std::isfinite(a.value) && std::abs(a.value) <= kMaxDodgeDegrees;
  return id >= 1 && id <= 15 && (a.mode == 1 || a.mode == 2 || a.mode == 4) &&
         std::isfinite(a.value) && std::abs(a.value) <= 100;
}
// Native committed effects, calculated once by the victim's original RPG engine.
// Numeric/GUID values only; native pointers, Soul IDs and event allocations never travel.
struct NativeEffectRecord {
  // 1 HealthValue, 2 ActStaminaValue, 3 AddBuff, 4 ItemHealthValue (the wear of a piece the
  // role's fighter wears; value: its item class id, then the health change as a float at + 16),
  // 5 ItemDestroyed (the break of such a piece; value: its item class id only)
  std::uint8_t kind{};
  std::uint8_t soul_role{};  // 1 attacker, 2 victim
  std::array<std::uint8_t, 48> value{};
};
using NativeEffects = std::vector<NativeEffectRecord>;
// The largest health change one item wear effect may carry (an item's health is 0 .. 1).
constexpr float kMaxItemWear = 1.0f;
inline bool valid_native_effects(const NativeEffects& effects) {
  if (effects.empty() || effects.size() > 16)
    return false;
  for (const auto& e : effects) {
    if (e.kind < 1 || e.kind > 5 || e.soul_role < 1 || e.soul_role > 2)
      return false;
    float number = 0;
    if (e.kind == 5) {
      for (unsigned i = 16; i < 48; ++i)
        if (e.value[i])
          return false;
    } else if (e.kind == 4) {
      std::memcpy(&number, e.value.data() + 16, 4);
      if (!std::isfinite(number) || std::abs(number) > kMaxItemWear)
        return false;
      for (unsigned i = 20; i < 48; ++i)
        if (e.value[i])
          return false;
    } else if (e.kind < 3) {
      std::uint32_t attribute = 0;
      std::memcpy(&attribute, e.value.data(), 4);
      std::memcpy(&number, e.value.data() + 4, 4);
      if (attribute != (e.kind == 1 ? 0u : 1u) || !std::isfinite(number) || e.value[16] > 2)
        return false;
      for (unsigned i = 9; i < 12; ++i)
        if (e.value[i])
          return false;
      for (unsigned i = 17; i < 48; ++i)
        if (e.value[i])
          return false;
    } else {
      if (e.value[32] > 3 || e.value[44] > 1 || e.value[45] > 1)
        return false;
      for (unsigned i = 33; i < 36; ++i)
        if (e.value[i])
          return false;
      if (e.value[32] != 2 && e.value[32] != 3 && e.value[45])
        return false;
      for (unsigned i = 46; i < 48; ++i)
        if (e.value[i])
          return false;
      std::memcpy(&number, e.value.data() + 36, 4);
      if (e.value[32] != 3 && !std::isfinite(number))
        return false;
    }
  }
  return true;
}
// S_HitInfo + S_AttackParams + the first 16 scalar context bytes + up to
// eight 16-byte native modifier records. No context/vector/movement pointers.
using NativeContactData = std::array<std::uint8_t, 470>;
inline bool valid_contact_data(const NativeContactData& data) {
  NativeReactionData info{};
  std::copy_n(data.begin(), 160, info.begin());
  if (!valid_reaction_data(info) || data[304] > 1 || data[305] > 1 || data[306] > 1 ||
      data[307] > 1 || data[320] > 8)
    return false;
  for (unsigned i = 296; i < 304; ++i)
    if (data[i])
      return false;  // native padding
  if (data[469] > 1)
    return false;  // original outcome boolean
  for (unsigned i = 160; i < 172; ++i)
    if (data[i])
      return false;  // unarmed instance/entity
  for (unsigned i = 196; i < 200; ++i)
    if (data[i])
      return false;  // target Cry entity
  for (unsigned at : {172u, 176u, 180u, 184u, 188u, 192u, 200u, 204u, 208u, 212u, 216u,
                      220u, 224u, 228u, 232u, 236u, 240u, 244u, 248u, 252u, 256u, 284u}) {
    float value;
    std::memcpy(&value, data.data() + at, 4);
    if (!std::isfinite(value))
      return false;
  }
  float power = 0;
  std::memcpy(&power, data.data() + 449, 4);
  if (!std::isfinite(power))
    return false;
  for (unsigned i = 0; i < data[320]; ++i)
    for (unsigned at : {325u + i * 16, 329u + i * 16}) {
      float value;
      std::memcpy(&value, data.data() + at, 4);
      if (!std::isfinite(value))
        return false;
    }
  return true;
}
struct MeleeEvent {
  std::uint32_t revision{};
  core::EntityId source{}, target{};
  std::uint8_t source_epoch{}, target_epoch{};
  std::uint64_t sequence{};
  bool executor_is_target = false;
  bool authority_is_target = false;
  NativeMeleeData data{};
  bool effects = false;
  NativeEffects effect_data{};
  bool contact = false;
  NativeContactData contact_data{};
  bool input = false;
  CombatActionData input_data{};
  // KCM11: an input sent once for every player who sees the source; the server addresses a copy
  // to each of them (as KCM3). `target` is the source's locked opponent, 0 for none.
  bool to_viewers = false;
  bool sync_hit = false;
  NativeSyncHitData sync_hit_data{};
  bool reaction = false;
  NativeReactionData reaction_data{};
  // KCM9 announces an original native attack start before its relayed input.
  // KCM10 binds a physical contact to that exact owner-generated lifecycle.
  bool attack_generation_marker = false;
  std::uint64_t attack_generation = 0;
  std::uint8_t native_attack_kind = 0;
};
inline std::vector<std::uint8_t> encode_melee(const MeleeEvent& e) {
  if (e.attack_generation_marker &&
      (!e.attack_generation || (e.native_attack_kind != 3 && e.native_attack_kind != 4) ||
       e.authority_is_target || e.executor_is_target || e.contact || e.effects || e.input ||
       e.sync_hit || e.reaction)) {
    return {};
  }
  if ((!e.contact && !e.attack_generation_marker && e.attack_generation) ||
      (!e.attack_generation_marker && e.native_attack_kind)) {
    return {};
  }
  if (e.contact && (e.authority_is_target || e.input || e.sync_hit || e.reaction || e.effects ||
                    e.executor_is_target || !valid_contact_data(e.contact_data)))
    return {};
  if (e.effects && (!e.authority_is_target || e.input || e.sync_hit || e.reaction || e.contact ||
                    !valid_native_effects(e.effect_data)))
    return {};
  if (e.authority_is_target && (e.input || e.sync_hit))
    return {};
  if (e.sync_hit &&
      (e.input || e.reaction || e.executor_is_target || !valid_sync_hit_data(e.sync_hit_data)))
    return {};
  if (e.to_viewers &&
      (!e.input || e.authority_is_target || (!e.target && e.target_epoch) || e.attack_generation))
    return {};
  // A viewers' input may name no opponent; every other event names its target.
  const bool target_named = e.target || e.to_viewers;
  if (!e.revision || !e.source || !target_named || e.source == e.target || !e.sequence ||
      (e.input ? (!valid_combat_action(e.input_data) || e.reaction || e.executor_is_target)
               : (e.reaction ? (!valid_reaction_data(e.reaction_data) || e.executor_is_target)
                             : (!e.sync_hit && !e.contact && !e.attack_generation_marker &&
                                !(e.effects ? valid_committed_melee_data(e.data)
                                            : valid_melee_data(e.data))))))
    return {};
  const std::uint8_t tag =
      e.attack_generation_marker
          ? 9
          : (e.contact ? (e.attack_generation ? 10 : 7)
                       : (e.effects ? 8
                                    : (e.sync_hit ? 5
                                                  : (e.input ? (e.to_viewers ? 11 : 3)
                                                             : (e.reaction ? 2 : 1)))));
  std::vector<std::uint8_t> out{'K', 'C', 'M', tag};
  auto put = [&](std::uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
      out.push_back(std::uint8_t(v >> (8 * i)));
  };
  put(e.revision, 4);
  put(e.source, 4);
  put(e.target, 4);
  put(e.source_epoch, 1);
  put(e.target_epoch, 1);
  put(e.sequence, 8);
  put((e.executor_is_target ? 1 : 0) | (e.authority_is_target ? 2 : 0), 1);
  if (e.attack_generation_marker) {
    put(e.attack_generation, 8);
    put(e.native_attack_kind, 1);
  } else if (e.contact) {
    if (e.attack_generation) {
      put(e.attack_generation, 8);
    }
    out.insert(out.end(), e.contact_data.begin(), e.contact_data.end());
  } else if (e.effects) {
    out.insert(out.end(), e.data.begin(), e.data.end());
    put(e.effect_data.size(), 1);
    for (const auto& effect : e.effect_data) {
      put(effect.kind, 1);
      put(effect.soul_role, 1);
      out.insert(out.end(), effect.value.begin(), effect.value.end());
    }
  } else if (e.sync_hit)
    out.insert(out.end(), e.sync_hit_data.begin(), e.sync_hit_data.end());
  else if (e.input) {
    put(unsigned(e.input_data.action), 1);
    put(e.input_data.mode, 1);
    std::uint32_t bits;
    std::memcpy(&bits, &e.input_data.value, 4);
    put(bits, 4);
  } else if (e.reaction)
    out.insert(out.end(), e.reaction_data.begin(), e.reaction_data.end());
  else
    out.insert(out.end(), e.data.begin(), e.data.end());
  return out;
}
inline bool decode_melee(const std::vector<std::uint8_t>& bytes, MeleeEvent& out) {
  if (bytes.size() < 4 || bytes[0] != 'K' || bytes[1] != 'C' || bytes[2] != 'M')
    return false;
  if ((bytes[3] != 1 || bytes.size() != 171) && (bytes[3] != 2 || bytes.size() != 200) &&
      (bytes[3] != 3 || bytes.size() != 33) && (bytes[3] != 11 || bytes.size() != 33) &&
      (bytes[3] != 5 || bytes.size() != 148) && (bytes[3] != 7 || bytes.size() != 497) &&
      (bytes[3] != 9 || bytes.size() != 36) && (bytes[3] != 10 || bytes.size() != 505) &&
      ((bytes[3] != 6 && bytes[3] != 8) || bytes.size() < 222 || bytes.size() > 972 ||
       (bytes.size() - 172) % 50))
    return false;
  std::size_t at = 4;
  auto get = [&](unsigned n) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
      v |= std::uint64_t(bytes[at++]) << (8 * i);
    return v;
  };
  MeleeEvent e;
  e.revision = std::uint32_t(get(4));
  e.source = core::EntityId(get(4));
  e.target = core::EntityId(get(4));
  e.source_epoch = std::uint8_t(get(1));
  e.target_epoch = std::uint8_t(get(1));
  e.sequence = get(8);
  auto role = get(1);
  if (role > 3)
    return false;
  e.executor_is_target = (role & 1) != 0;
  e.authority_is_target = (role & 2) != 0;
  e.contact = bytes[3] == 7 || bytes[3] == 10;
  e.effects = bytes[3] == 6 || bytes[3] == 8;
  e.sync_hit = bytes[3] == 5;
  e.input = bytes[3] == 3 || bytes[3] == 11;
  e.to_viewers = bytes[3] == 11;
  e.reaction = bytes[3] == 2;
  e.attack_generation_marker = bytes[3] == 9;
  if (e.attack_generation_marker) {
    e.attack_generation = get(8);
    e.native_attack_kind = std::uint8_t(get(1));
  } else if (e.contact) {
    if (bytes[3] == 10) {
      e.attack_generation = get(8);
      if (!e.attack_generation) {
        return false;
      }
    }
    std::copy(bytes.begin() + at, bytes.end(), e.contact_data.begin());
  } else if (e.effects) {
    std::copy_n(bytes.begin() + 27, 144, e.data.begin());
    if (bytes[3] == 6 && !valid_melee_data(e.data)) {
      return false;
    }
    at = 171;
    auto count = get(1);
    if (count > 16 || bytes.size() != 172 + count * 50)
      return false;
    for (unsigned i = 0; i < count; ++i) {
      NativeEffectRecord effect;
      effect.kind = std::uint8_t(get(1));
      effect.soul_role = std::uint8_t(get(1));
      std::copy_n(bytes.begin() + at, 48, effect.value.begin());
      at += 48;
      e.effect_data.push_back(effect);
    }
  } else if (e.sync_hit)
    std::copy(bytes.begin() + 27, bytes.end(), e.sync_hit_data.begin());
  else if (e.input) {
    e.input_data.action = CombatAction(get(1));
    e.input_data.mode = std::uint8_t(get(1));
    auto bits = std::uint32_t(get(4));
    std::memcpy(&e.input_data.value, &bits, 4);
  } else if (e.reaction)
    std::copy(bytes.begin() + 27, bytes.end(), e.reaction_data.begin());
  else
    std::copy(bytes.begin() + 27, bytes.end(), e.data.begin());
  if (encode_melee(e).empty())
    return false;
  out = e;
  return true;
}
}  // namespace kcd2s::integration
