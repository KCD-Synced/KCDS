#include "xp_gate.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"

namespace kcds::runtime::progression::xp_gate {
namespace {

namespace integration = kcd2s::integration;

using Apply = char(__fastcall*)(void* effect);

constexpr std::size_t kEffectEvent = 8;
constexpr std::size_t kEventCause = 8;
constexpr std::size_t kEffectSoul = 16;
constexpr std::size_t kEffectTrack = 24;
constexpr std::size_t kEffectXp = 28;
constexpr std::size_t kSkillMultipliers = 56;
constexpr std::size_t kStatMultipliers = 52;
constexpr std::size_t kSoulId = 48;
// Gains held back between two frames, and the server's own effects remembered at once: far more
// than a frame produces; beyond it the oldest go.
constexpr std::size_t kMaxHeld = 256;
constexpr std::size_t kMaxAllowed = 64;

Logger log_line = nullptr;
Natives natives;
Apply original_skill_apply = nullptr;
Apply original_stat_apply = nullptr;
std::uint64_t local_soul_id = 0;
bool native_xp = true;
std::vector<void*> allowed;
std::vector<integration::XpGain> held;

template <class T>
T field(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return value;
}

// The kTracks index of a skill or stat id, or -1 for one players do not level.
int track_of(bool stat, std::uint32_t id) {
  for (std::size_t i = 0; i < integration::kTracks.size(); ++i) {
    if (integration::kTracks[i].stat == stat && integration::kTracks[i].id == id) {
      return int(i);
    }
  }
  return -1;
}

// The game's name of what caused the effect: the cause's class (".?AVC_AttackCause@rpgmodule@wh@@"
// is "Attack"), read from its RTTI; empty for anything else.
std::string source_of(const void* effect) {
  const void* event = field<const void*>(effect, kEffectEvent);
  const void* cause = event ? field<const void*>(event, kEventCause) : nullptr;
  if (!cause || !natives.image) {
    return {};
  }
  const auto* vtable = field<const std::uint8_t* const*>(cause, 0);
  if (!vtable) {
    return {};
  }
  const auto* locator = reinterpret_cast<const std::uint8_t*>(vtable[-1]);
  if (!locator || field<std::uint32_t>(locator, 0) != 1) {
    return {};
  }
  const auto* name =
      reinterpret_cast<const char*>(natives.image + field<std::uint32_t>(locator, 12) + 16);
  constexpr char kPrefix[] = ".?AVC_";
  constexpr char kSuffix[] = "Cause@rpgmodule@wh@@";
  const std::string text(name, strnlen(name, 128));
  if (text.rfind(kPrefix, 0) != 0) {
    return {};
  }
  const std::size_t end = text.find(kSuffix);
  if (end == std::string::npos || end <= sizeof(kPrefix) - 1) {
    return {};
  }
  std::string source = text.substr(sizeof(kPrefix) - 1, end - (sizeof(kPrefix) - 1));
  return source.size() <= integration::kMaxXpSourceBytes ? source : std::string();
}

// Whether the effect is held back (true) or applied as the game would (false).
bool hold(void* effect, bool stat) {
  if (!local_soul_id || field<std::uint64_t>(effect, kEffectSoul) != local_soul_id) {
    return false;
  }
  auto own = std::find(allowed.begin(), allowed.end(), effect);
  if (own != allowed.end()) {
    allowed.erase(own);
    return false;
  }
  const int track = track_of(stat, field<std::uint32_t>(effect, kEffectTrack));
  const std::int32_t xp = field<std::int32_t>(effect, kEffectXp);
  if (track < 0 || xp <= 0) {
    return false;
  }
  if (native_xp) {
    integration::XpGain gain;
    gain.track = std::uint8_t(track);
    gain.xp = std::uint32_t(std::min<std::int64_t>(xp, integration::kMaxOrderXp));
    gain.multipliers =
        field<std::uint8_t>(effect, stat ? kStatMultipliers : kSkillMultipliers) != 0;
    gain.source = source_of(effect);
    if (held.size() >= kMaxHeld) {
      held.erase(held.begin());
    }
    held.push_back(std::move(gain));
  }
  return true;
}

// A held-back effect answers 1 (applied), as the game's own Apply does when it grants nothing (its
// multiplier at 0). The game applies all effects of an event together and refuses the whole event
// when one answers 0: a vault's stat XP answering 0 refused the vault itself.
char __fastcall hooked_skill_apply(void* effect) {
  return hold(effect, false) ? 1 : original_skill_apply(effect);
}

char __fastcall hooked_stat_apply(void* effect) {
  return hold(effect, true) ? 1 : original_stat_apply(effect);
}

}  // namespace

bool install(const Natives& resolved, Logger logger) {
  log_line = logger;
  natives = resolved;
  if (!natives.skill_apply || !natives.stat_apply || !natives.image) {
    return false;
  }
  return MH_CreateHook(natives.skill_apply, reinterpret_cast<void*>(hooked_skill_apply),
                       reinterpret_cast<void**>(&original_skill_apply)) == MH_OK &&
         MH_EnableHook(natives.skill_apply) == MH_OK &&
         MH_CreateHook(natives.stat_apply, reinterpret_cast<void*>(hooked_stat_apply),
                       reinterpret_cast<void**>(&original_stat_apply)) == MH_OK &&
         MH_EnableHook(natives.stat_apply) == MH_OK;
}

void tick(void* soul, bool allow_native_xp) {
  local_soul_id = soul ? field<std::uint64_t>(soul, kSoulId) : 0;
  native_xp = allow_native_xp;
  if (!soul) {
    held.clear();
    allowed.clear();
  }
}

void allow(void* effect) {
  if (allowed.size() >= kMaxAllowed) {
    allowed.erase(allowed.begin());
  }
  allowed.push_back(effect);
}

std::vector<integration::XpGain> take_gains() {
  if (log_line && !held.empty()) {
    log_line("PROGRESSION xp held back for the server: %zu (first track=%u xp=%u source=%s)",
             held.size(), unsigned(held.front().track), held.front().xp,
             held.front().source.c_str());
  }
  return std::exchange(held, {});
}

}  // namespace kcds::runtime::progression::xp_gate
