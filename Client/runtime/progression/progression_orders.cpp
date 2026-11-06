#include "progression_orders.hpp"
#include "progression_report.hpp"
#include "kcd2s/integration/combat_capabilities.hpp"

#include "perk_gate.hpp"
#include "xp_gate.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"
#include "../hud/level_up_notice.hpp"

namespace kcds::runtime::progression {
namespace {

namespace integration = kcd2s::integration;
using Kind = integration::ProgressionOrder::Kind;

using CreateCause = void**(__fastcall*)(void* unused, void** handle, const std::uint64_t* soul_id);
using ReleaseCause = void(__fastcall*)(void** handle);
using CreateXpEffect = std::uint8_t*(__fastcall*)(void* unused, void* cause, std::uint64_t soul_id,
                                                  int track, const int* xp);
using SubmitCause = void(__fastcall*)(void* queue, void* cause);
using RpgParams = void*(__fastcall*)();
using SkillTotal = int*(__fastcall*)(void* params, int* total, int level);
using StatTotal = int*(__fastcall*)(void* params, int* total, int level, int stat);
using AddPerk = bool(__fastcall*)(void* perk_set, const integration::PerkId* id,
                                  std::uint64_t source);
using RemovePerk = void(__fastcall*)(void* perk_set, const integration::PerkId* id);
using UpdatePerks = bool(__fastcall*)(void* perk_set);
using GetBuffManager = void*(__fastcall*)();
using AddBuff = void*(__fastcall*)(void* manager, void* soul, const void* id, void* source,
                                   const void* params);
using PointsListener = std::int64_t(__fastcall*)(void* listener, void* effect);
using SkillChanged = void(__fastcall*)(void* skills, unsigned skill);
using StatChanged = void(__fastcall*)(void* listeners, int stat);

constexpr std::size_t kSoulId = 48;
constexpr std::size_t kSkills = 1512;
constexpr std::size_t kStats = 1388;
constexpr std::size_t kPerkSet = 1880;
constexpr std::size_t kPerkPoints = 88;
constexpr std::size_t kStatPoints = 16;
constexpr std::size_t kSkillPoints = 36;
constexpr std::size_t kMainPoints = 106;
constexpr std::size_t kRpgQueue = 0xB0;
constexpr std::size_t kSkillMultipliers = 56;
constexpr std::size_t kStatMultipliers = 52;
// The soul's attribute-change listeners, which the stat XP effect tells of a stat's change.
constexpr std::size_t kChangeListeners = 88;
constexpr std::size_t kSubmitCause = 0;
constexpr std::size_t kAddBuff = 0;
// rpg/buff.xml "respec" (c8b0d038-a503-44cc-85a5-7f753a09eb6e): the game's own respec.
constexpr std::array<std::uint8_t, 16> kRespecBuff = {
    0x38, 0xd0, 0xb0, 0xc8, 0x03, 0xa5, 0xcc, 0x44, 0x85, 0xa5, 0x7f, 0x75, 0x3a, 0x09, 0xeb, 0x6e};

Logger log_line = nullptr;
OrderNatives natives;
bool native_xp_allowed = true;

// A restore the game is still applying: its XP effects the perk-point listener has not taken
// yet, then perks and points.
struct PendingRestore {
  void* soul = nullptr;
  std::vector<void*> effects;
  integration::Progression snapshot;
  // { notify: false }: the levels it brings back are not announced.
  bool quiet = false;
};
std::optional<PendingRestore> pending;
PointsListener original_skill_points = nullptr;
PointsListener original_stat_points = nullptr;

// The listener took an effect: the restore stops waiting for it.
void taken(void* effect) {
  if (!pending) {
    return;
  }
  auto& effects = pending->effects;
  effects.erase(std::remove(effects.begin(), effects.end(), effect), effects.end());
}

std::int64_t __fastcall hooked_skill_points(void* listener, void* effect) {
  const std::int64_t result = original_skill_points(listener, effect);
  taken(effect);
  return result;
}

std::int64_t __fastcall hooked_stat_points(void* listener, void* effect) {
  const std::int64_t result = original_stat_points(listener, effect);
  taken(effect);
  return result;
}

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

std::uint8_t* bytes(void* pointer) {
  return static_cast<std::uint8_t*>(pointer);
}

void* perk_set(void* soul) {
  return bytes(soul) + kPerkSet;
}

// The track's level and XP towards the next level (fixed point).
std::pair<int, int> track_state(void* soul, integration::TrackId track) {
  const std::size_t pair = (track.stat ? kStats : kSkills) + 8 * std::size_t(track.id);
  int level = 0;
  int xp = 0;
  std::memcpy(&level, bytes(soul) + pair, 4);
  std::memcpy(&xp, bytes(soul) + pair + 4, 4);
  return {level, xp};
}

// The game's total XP from level 0 to `level` (fixed point).
std::int64_t total_xp(integration::TrackId track, int level) {
  void* params = reinterpret_cast<RpgParams>(natives.rpg_params)();
  int total = 0;
  if (track.stat) {
    reinterpret_cast<StatTotal>(natives.stat_total)(params, &total, level, track.id);
  } else {
    reinterpret_cast<SkillTotal>(natives.skill_total)(params, &total, level);
  }
  return total;
}

std::int64_t current_total(void* soul, integration::TrackId track) {
  const auto [level, xp] = track_state(soul, track);
  return total_xp(track, level) + xp;
}

// XP on a track through the game's own effect; multipliers as the player's perks give them.
// Returns the effect (nullptr when none was made).
void* grant_xp(void* soul, integration::TrackId track, std::uint32_t xp, bool multipliers) {
  void* rpg = *natives.rpg;
  if (!rpg || xp == 0) {
    return nullptr;
  }
  void* queue = *reinterpret_cast<void**>(bytes(rpg) + kRpgQueue);
  std::uint64_t soul_id = 0;
  std::memcpy(&soul_id, bytes(soul) + kSoulId, 8);
  void* cause = nullptr;
  reinterpret_cast<CreateCause>(natives.create_cause)(nullptr, &cause, &soul_id);
  if (!cause) {
    return nullptr;
  }
  const int amount = int(std::min<std::uint32_t>(xp, 0x7FFFFFFF));
  auto create = reinterpret_cast<CreateXpEffect>(track.stat ? natives.stat_xp : natives.skill_xp);
  std::uint8_t* effect = create(nullptr, cause, soul_id, track.id, &amount);
  if (effect) {
    // The server's own XP: the gate lets it through.
    xp_gate::allow(effect);
    if (multipliers) {
      effect[track.stat ? kStatMultipliers : kSkillMultipliers] = 1;
    }
  }
  slot<SubmitCause>(queue, kSubmitCause)(queue, cause);
  reinterpret_cast<ReleaseCause>(natives.release_cause)(&cause);
  return effect;
}

void set_level(void* soul, integration::TrackId track, int level) {
  const int current = track_state(soul, track).first;
  if (level <= current) {
    return;
  }
  const std::int64_t xp = total_xp(track, level) - total_xp(track, current);
  grant_xp(soul, track, std::uint32_t(std::max<std::int64_t>(xp, 0)), false);
}

// The exact order: the track's level and its XP towards the next level (fixed
// point) written as they are, then the game told of the change as its XP add tells it, without
// an XP effect: no perk points, no level-up notice, no XP gain. The XP stays below what the next
// level costs (none at the cap).
void set_track(void* soul, integration::TrackId track, int level, std::int64_t xp) {
  level = std::clamp(level, 0, int(integration::kLevelCap));
  const std::int64_t cost =
      level >= integration::kLevelCap ? 0 : total_xp(track, level + 1) - total_xp(track, level);
  xp = std::clamp<std::int64_t>(xp, 0, std::max<std::int64_t>(cost - 1, 0));
  const std::size_t pair = (track.stat ? kStats : kSkills) + 8 * std::size_t(track.id);
  const std::int32_t level_value = level;
  const std::int32_t xp_value = std::int32_t(xp);
  std::memcpy(bytes(soul) + pair, &level_value, 4);
  std::memcpy(bytes(soul) + pair + 4, &xp_value, 4);
  if (track.stat) {
    reinterpret_cast<StatChanged>(natives.stat_changed)(bytes(soul) + kChangeListeners, track.id);
  } else {
    reinterpret_cast<SkillChanged>(natives.skill_changed)(bytes(soul) + kSkills, track.id);
  }
}

std::uint16_t* points_counter(void* soul, std::uint8_t bank) {
  void* points = *reinterpret_cast<void**>(bytes(perk_set(soul)) + kPerkPoints);
  if (!points) {
    return nullptr;
  }
  std::size_t offset = kMainPoints;
  if (bank != integration::kMainBank) {
    const integration::TrackId track = integration::kTracks[bank];
    offset = (track.stat ? kStatPoints : kSkillPoints) + 2 * std::size_t(track.id);
  }
  return reinterpret_cast<std::uint16_t*>(bytes(points) + offset);
}

void set_points(void* soul, std::uint8_t bank, std::uint16_t value) {
  if (std::uint16_t* counter = points_counter(soul, bank)) {
    *counter = value;
  }
}

void add_points(void* soul, std::uint8_t bank, std::uint16_t count) {
  if (std::uint16_t* counter = points_counter(soul, bank)) {
    *counter = std::uint16_t(std::min<std::uint32_t>(std::uint32_t(*counter) + count, 0xFFFF));
    reinterpret_cast<UpdatePerks>(natives.update_perks)(perk_set(soul));
  }
}

bool owns(void* soul, const integration::PerkId& id) {
  const auto* set = bytes(perk_set(soul));
  void* const* begin = nullptr;
  void* const* end = nullptr;
  std::memcpy(&begin, set + 64, 8);
  std::memcpy(&end, set + 72, 8);
  for (auto it = begin; it && it != end; ++it) {
    if (*it && std::memcmp(bytes(*it) + 16, id.data(), id.size()) == 0) {
      return true;
    }
  }
  return false;
}

void respec(void* soul) {
  void* manager = reinterpret_cast<GetBuffManager>(natives.buff_manager)();
  if (!manager) {
    return;
  }
  const std::array<std::uint8_t, 16> params{};
  slot<AddBuff>(manager, kAddBuff)(manager, soul, kRespecBuff.data(), nullptr, params.data());
}

// The XP each track lacks to reach the snapshot; the perks and points follow once the game has
// processed it.
void restore(void* soul, const integration::Progression& snapshot, bool quiet) {
  PendingRestore restore_state;
  restore_state.soul = soul;
  restore_state.snapshot = snapshot;
  restore_state.quiet = quiet;
  pending = std::move(restore_state);
  for (std::size_t i = 0; i < integration::kTracks.size(); ++i) {
    const integration::TrackId track = integration::kTracks[i];
    const auto& wanted = snapshot.tracks[i];
    const std::int64_t missing =
        total_xp(track, wanted.level) + wanted.xp - current_total(soul, track);
    if (missing <= 0) {
      continue;
    }
    if (void* effect = grant_xp(soul, track, std::uint32_t(missing), false)) {
      pending->effects.push_back(effect);
    }
  }
}

// Once the perk-point listener has taken every XP effect of the restore: the perks it lacks, then
// the snapshot's unspent points (the levels just crossed granted theirs; the snapshot's are the
// true ones).
void finish_restore(void* soul) {
  if (!pending) {
    return;
  }
  if (pending->soul != soul) {
    pending.reset();
    return;
  }
  if (pending->quiet) {
    // Its levels are crossed as the game takes its XP effects, over a few frames.
    hud::quiet_level_ups();
  }
  if (!pending->effects.empty()) {
    return;
  }
  std::size_t added = 0;
  // The server's own perks: blocked ones too.
  perk_gate::ServerGrant grant;
  for (const auto& id : pending->snapshot.perks) {
    if (!owns(soul, id) && reinterpret_cast<AddPerk>(natives.add_perk)(perk_set(soul), &id, 0)) {
      ++added;
    }
  }
  for (std::size_t i = 0; i < integration::kTracks.size(); ++i) {
    set_points(soul, std::uint8_t(i), pending->snapshot.tracks[i].points);
  }
  set_points(soul, integration::kMainBank, pending->snapshot.main_points);
  reinterpret_cast<UpdatePerks>(natives.update_perks)(perk_set(soul));
  if (log_line) {
    log_line("PROGRESSION restore finished perks_added=%zu", added);
  }
  pending.reset();
}

// The perks the soul owns itself (its perk set's list, without the perks they grant with them).
std::vector<integration::PerkId> owned_perks(void* soul) {
  std::vector<integration::PerkId> perks;
  const auto* set = bytes(perk_set(soul));
  void* const* begin = nullptr;
  void* const* end = nullptr;
  std::memcpy(&begin, set + 64, 8);
  std::memcpy(&end, set + 72, 8);
  for (auto it = begin; it && it != end; ++it) {
    if (*it) {
      integration::PerkId id{};
      std::memcpy(id.data(), bytes(*it) + 16, id.size());
      perks.push_back(id);
    }
  }
  return perks;
}

// An exact restore: every track written as set_track writes it, the perks the
// snapshot lacks removed and its missing ones added, and its unspent points set. A perk another
// perk grants is in the snapshot but not in the soul's own list; it is neither removed nor added
// by itself.
void restore_exact(void* soul, const integration::Progression& snapshot) {
  pending.reset();
  for (std::size_t i = 0; i < integration::kTracks.size(); ++i) {
    const auto& wanted = snapshot.tracks[i];
    set_track(soul, integration::kTracks[i], wanted.level, wanted.xp);
  }
  std::size_t removed = 0;
  for (const auto& id : owned_perks(soul)) {
    if (!std::binary_search(snapshot.perks.begin(), snapshot.perks.end(), id)) {
      reinterpret_cast<RemovePerk>(natives.remove_perk)(perk_set(soul), &id);
      ++removed;
    }
  }
  std::size_t added = 0;
  {
    perk_gate::ServerGrant grant;
    integration::Progression now;
    read(soul, now);
    for (const auto& id : snapshot.perks) {
      const bool held = std::binary_search(now.perks.begin(), now.perks.end(), id);
      if (!held && reinterpret_cast<AddPerk>(natives.add_perk)(perk_set(soul), &id, 0)) {
        ++added;
      }
    }
  }
  for (std::size_t i = 0; i < integration::kTracks.size(); ++i) {
    set_points(soul, std::uint8_t(i), snapshot.tracks[i].points);
  }
  set_points(soul, integration::kMainBank, snapshot.main_points);
  reinterpret_cast<UpdatePerks>(natives.update_perks)(perk_set(soul));
  if (log_line) {
    log_line("PROGRESSION exact restore perks_removed=%zu perks_added=%zu", removed, added);
  }
}

void apply_order(void* soul, const integration::ProgressionOrder& order) {
  if (!order.notify) {
    hud::quiet_level_ups();
  }
  switch (order.kind) {
    case Kind::AddXp:
      grant_xp(soul, integration::kTracks[order.track], order.xp, order.multipliers);
      break;
    case Kind::SetLevel:
      set_level(soul, integration::kTracks[order.track], order.level);
      break;
    case Kind::AddPerk: {
      perk_gate::ServerGrant grant;
      const bool added =
          reinterpret_cast<AddPerk>(natives.add_perk)(perk_set(soul), &order.perk, 0);
      // False for a perk the game's perk table lacks (a server's own perk before a restart).
      if (log_line) {
        log_line("PROGRESSION perk add %s", added ? "taken" : "refused by the game");
      }
      break;
    }
    case Kind::RemovePerk:
      reinterpret_cast<RemovePerk>(natives.remove_perk)(perk_set(soul), &order.perk);
      break;
    case Kind::AddPoints:
      add_points(soul, order.track, order.count);
      break;
    case Kind::Respec:
      respec(soul);
      break;
    case Kind::Restore:
      if (order.exact) {
        restore_exact(soul, order.snapshot);
      } else {
        restore(soul, order.snapshot, !order.notify);
      }
      break;
    case Kind::SetTrack:
      set_track(soul, integration::kTracks[order.track], order.level, order.xp);
      break;
    case Kind::NativeXp:
      native_xp_allowed = order.level != 0;
      break;
    case Kind::LearnPerk:
      perk_gate::learn(soul, order.perk);
      break;
    case Kind::BlockPerks:
      perk_gate::set_blocked(order.perks);
      break;
    case Kind::RefusePerk:
      perk_gate::refused(order.perk, order.refusal);
      break;
  }
  if (log_line) {
    log_line("PROGRESSION order %u track=%u xp=%u level=%u count=%u perk=%s", unsigned(order.kind),
             unsigned(order.track), order.xp, unsigned(order.level), unsigned(order.count),
             integration::perk_id_text(order.perk).c_str());
  }
}

}  // namespace

bool install(const OrderNatives& resolved, Logger logger) {
  log_line = logger;
  natives = resolved;
  if (!natives.rpg || !natives.create_cause || !natives.release_cause || !natives.skill_xp ||
      !natives.stat_xp || !natives.rpg_params || !natives.skill_total || !natives.stat_total ||
      !natives.add_perk || !natives.remove_perk || !natives.update_perks || !natives.buff_manager ||
      !natives.skill_points || !natives.stat_points || !natives.skill_changed ||
      !natives.stat_changed) {
    return false;
  }
  return MH_CreateHook(natives.skill_points, reinterpret_cast<void*>(hooked_skill_points),
                       reinterpret_cast<void**>(&original_skill_points)) == MH_OK &&
         MH_EnableHook(natives.skill_points) == MH_OK &&
         MH_CreateHook(natives.stat_points, reinterpret_cast<void*>(hooked_stat_points),
                       reinterpret_cast<void**>(&original_stat_points)) == MH_OK &&
         MH_EnableHook(natives.stat_points) == MH_OK;
}

void apply(void* soul, const std::vector<integration::ProgressionOrder>& orders) {
  if (!soul) {
    pending.reset();
    native_xp_allowed = true;
    return;
  }
  for (const auto& order : orders) {
    apply_order(soul, order);
  }
  finish_restore(soul);
}

bool native_xp() {
  return native_xp_allowed;
}

bool mirror_riposte_ability(void* replica_soul, bool enabled) {
  integration::Progression current;
  if (!natives.add_perk || !natives.remove_perk || !read(replica_soul, current)) {
    return false;
  }
  if (integration::owns_riposte_ability(current) == enabled) {
    return true;
  }
  if (enabled) {
    if (!reinterpret_cast<AddPerk>(natives.add_perk)(perk_set(replica_soul),
                                                     &integration::kRiposteAbilityPerk, 0)) {
      return false;
    }
  } else {
    reinterpret_cast<RemovePerk>(natives.remove_perk)(perk_set(replica_soul),
                                                      &integration::kRiposteAbilityPerk);
  }
  return read(replica_soul, current) && integration::owns_riposte_ability(current) == enabled;
}

}  // namespace kcds::runtime::progression
