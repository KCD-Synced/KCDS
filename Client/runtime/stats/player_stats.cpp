#include "player_stats.hpp"

#include "../knockout/knockout.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace kcds::runtime::stats {
namespace {

namespace integration = kcd2s::integration;

using DerivedStat = float(__fastcall*)(void* soul, int id, void* context);
using StatLevelRelative = float(__fastcall*)(void* soul, int stat, void* context);
using SetState = void(__fastcall*)(void* soul, unsigned state, float value);
using StateMax = float(__fastcall*)(void* soul, unsigned state);
using StatLevel = float(__fastcall*)(void* soul, int id, void* context);
using RpgParams = const void*(__fastcall*)();
using DamageTypeInit = void*(__fastcall*)(void* type);
using DamageTypeFree = void(__fastcall*)(void* type);
using DealDamage = void(__fastcall*)(void* damage, float stamina, float health, void* attacker,
                                     char flag, void* type);

constexpr std::size_t kSoulStates = 1800;
constexpr std::size_t kSoulDamage = 240;
constexpr std::size_t kDealDamage = 27;
constexpr unsigned kHealthState = 0;

// The game's derived-stat ids of the five conditions, in kVitalNames order after the states.
constexpr std::array<int, 5> kConditionIds = {
    35,  // bleeding
    54,  // sleeping
    55,  // consciousness
    63,  // drunkenness
    64,  // poisoning
};
static_assert(kConditionIds.size() + integration::kWritableStateCount ==
              integration::kVitalNames.size());

// The game's derived-stat ids of the sampled stats, in kDerivedNames order.
constexpr std::array<int, integration::kDerivedNames.size()> kDerivedIds = {
    0,    // charisma
    3,    // visibility
    5,    // conspicuousness
    29,   // noise
    72,   // dirtiness
    88,   // bloodiness
    73,   // smell
    74,   // smellIntensity
    75,   // fragrance
    40,   // carriedWeight
    23,   // inventoryCapacity
    77,   // encumbrance
    113,  // hangover
    109,  // alcoholism
    90,   // armorRating
    25,   // overallArmorDefense
    26,   // overallWeaponAttack
    31,   // normalizedRunSpeed
    32,   // runSpeedBase
    42,   // morale
};

// The game's attribute ids of the relative attributes (strength, agility, vitality), in
// kRelativeNames order.
constexpr std::array<int, integration::kRelativeNames.size()> kAttributeIds = {0, 1, 2};

// The game's derived-stat ids of the stamina the body's injuries allow and of the main level.
constexpr int kHealthyStaminaId = 7;
constexpr int kMainLevelId = 28;
// The game's skill ids of SoulSkills (rpg/skill.xml), in kSkillNames order.
constexpr std::array<int, integration::kSkillNames.size()> kSkillIds = {
    2,   // fencing
    14,  // survival
    15,  // defense
    16,  // weapon_sword
    17,  // heavy_weapons
    19,  // marksmanship
    22,  // weapon_dagger
    23,  // weapon_large
    24,  // weapon_unarmed
};
// The highest skill level in the RPG parameters.
constexpr std::size_t kMaxSkillLevel = 180;

// Stamina, exhaustion and hunger are published at most this often. Health and conditions are
// published as they change, so a hit does not wait behind the slower state sampling period.
constexpr std::uint64_t kStatePeriodMs = 100;
// The other derived stats and the relative attributes are sampled this often.
constexpr std::uint64_t kSamplePeriodMs = 250;

Logger log_line = nullptr;
Natives natives;
Fields fields;
integration::PlayerVitals published_vitals;
std::uint64_t vitals_published_at = 0;
std::uint64_t sampled_at = 0;
bool sampled = false;

// The evaluation context the script binding passes: no opponent, no weapon.
struct Context {
  alignas(16) std::uint8_t bytes[64] = {};
};

float derived(void* soul, int id) {
  Context context;
  return reinterpret_cast<DerivedStat>(natives.derived_stat)(soul, id, context.bytes);
}

float relative(void* soul, int attribute) {
  Context context;
  return reinterpret_cast<StatLevelRelative>(natives.stat_level_relative)(soul, attribute,
                                                                          context.bytes);
}

float state_max(void* soul, unsigned index) {
  return reinterpret_cast<StateMax>(natives.state_max)(soul, index);
}

float stat_level(void* soul, int stat) {
  Context context;
  return reinterpret_cast<StatLevel>(natives.stat_level)(soul, stat, context.bytes);
}

float skill_level(void* soul, int skill) {
  Context context;
  return reinterpret_cast<StatLevel>(natives.skill_level)(soul, skill, context.bytes);
}

float state(void* soul, unsigned index) {
  return *reinterpret_cast<const float*>(static_cast<std::uint8_t*>(soul) + kSoulStates +
                                         4 * index);
}

integration::PlayerVitals read_vitals(void* soul) {
  integration::PlayerVitals vitals;
  for (unsigned i = 0; i < integration::kWritableStateCount; ++i) {
    vitals.values[i] = integration::stat_field_value(state(soul, i));
  }
  for (std::size_t i = 0; i < kConditionIds.size(); ++i) {
    vitals.values[integration::kWritableStateCount + i] =
        integration::stat_field_value(derived(soul, kConditionIds[i]));
  }
  return vitals;
}

bool conditions_differ(const integration::PlayerVitals& a, const integration::PlayerVitals& b) {
  for (std::size_t i = integration::kWritableStateCount; i < a.values.size(); ++i) {
    if (a.values[i] != b.values[i]) {
      return true;
    }
  }
  return false;
}

integration::PlayerStatsSample read_sample(void* soul) {
  integration::PlayerStatsSample sample;
  for (std::size_t i = 0; i < kDerivedIds.size(); ++i) {
    sample.derived[i] = derived(soul, kDerivedIds[i]);
  }
  for (std::size_t i = 0; i < kAttributeIds.size(); ++i) {
    sample.relative[i] = relative(soul, kAttributeIds[i]);
  }
  return sample;
}

integration::PlayerAbilities read_abilities(void* soul) {
  integration::PlayerAbilities abilities;
  for (unsigned i = 0; i < integration::kWritableStateCount; ++i) {
    // maxima: health, stamina, then staminaCap, then exhaust and hunger.
    const std::size_t slot = i < 2 ? i : i + 1;
    abilities.maxima[slot] = state_max(soul, i);
  }
  abilities.maxima[2] = derived(soul, kHealthyStaminaId);
  abilities.level = derived(soul, kMainLevelId);
  for (std::size_t i = 0; i < kAttributeIds.size(); ++i) {
    abilities.attributes[i] = stat_level(soul, kAttributeIds[i]);
  }
  const auto* params =
      static_cast<const std::uint8_t*>(reinterpret_cast<RpgParams>(natives.rpg_params)());
  std::int32_t highest = 0;
  if (params) {
    std::memcpy(&highest, params + kMaxSkillLevel, sizeof(highest));
  }
  for (std::size_t i = 0; i < kSkillIds.size(); ++i) {
    const float level = skill_level(soul, kSkillIds[i]);
    abilities.skills[i] = level;
    abilities.relative_skills[i] = highest > 0 ? level / float(highest) : 0.0f;
  }
  return abilities;
}

// The value an addStat request asks for: the state's current value plus the change, within 0 and
// the state's maximum (the one the setter clamps to). Positive health the setter would take for
// zero counts as zero, so the request ends in the game's own lethal damage as setStat 0 does.
float changed_value(void* soul, const integration::StatRequest& request) {
  const float maximum = state_max(soul, request.stat);
  const float wanted = std::fmin(std::fmax(state(soul, request.stat) + request.value, 0.0f),
                                 std::fmax(maximum, 0.0f));
  if (request.stat == kHealthState && wanted <= integration::kMinPositiveHealth) {
    return 0.0f;
  }
  return wanted;
}

// The game's own lethal damage: the knock-out of a server world follows.
void deal_lethal_damage(void* soul) {
  void* damage = static_cast<std::uint8_t*>(soul) + kSoulDamage;
  alignas(16) std::uint8_t type[64] = {};
  reinterpret_cast<DamageTypeInit>(natives.damage_type_init)(type);
  const float health = state(soul, kHealthState) + 1.0f;
  auto deal = reinterpret_cast<DealDamage>((*reinterpret_cast<void***>(damage))[kDealDamage]);
  deal(damage, 0.0f, health, nullptr, 0, type);
  reinterpret_cast<DamageTypeFree>(natives.damage_type_free)(type);
}

}  // namespace

bool install(const Natives& resolved, Logger logger) {
  log_line = logger;
  natives = resolved;
  return natives.derived_stat && natives.stat_level_relative && natives.set_state &&
         natives.damage_type_init && natives.damage_type_free && natives.state_max &&
         natives.stat_level && natives.skill_level && natives.rpg_params;
}

void deal_lethal(void* soul) {
  if (soul && natives.damage_type_init && natives.damage_type_free) {
    deal_lethal_damage(soul);
  }
}

const Fields& capture(void* soul, std::uint64_t now_ms) {
  if (!soul || !natives.derived_stat) {
    fields = {};
    published_vitals = {};
    sampled = false;
    return fields;
  }
  integration::PlayerVitals vitals = read_vitals(soul);
  // Kept down after a knock-out, the player lies for everyone until revived, also in the moment its
  // game ends the unconsciousness by itself (the replica stood up for two seconds).
  constexpr std::size_t kConsciousness = 6;
  static_assert(std::string_view(integration::kVitalNames[kConsciousness]) == "consciousness");
  if (kcds::runtime::knockout::kept_down_now()) {
    vitals.values[kConsciousness] = 0.0f;
  }
  const bool first = fields.vitals.empty();
  const bool health_changed = vitals.values[kHealthState] != published_vitals.values[kHealthState];
  const bool states_due = now_ms - vitals_published_at >= kStatePeriodMs;
  if (first || health_changed || conditions_differ(vitals, published_vitals) ||
      (states_due && !(vitals == published_vitals))) {
    fields.vitals = integration::vitals_field(vitals);
    published_vitals = vitals;
    vitals_published_at = now_ms;
  }
  if (!sampled || now_ms - sampled_at >= kSamplePeriodMs) {
    fields.stats = integration::stats_field(read_sample(soul));
    fields.abilities = integration::abilities_field(read_abilities(soul));
    sampled_at = now_ms;
    sampled = true;
  }
  return fields;
}

void apply(void* soul, const std::vector<integration::StatRequest>& requests, bool knocked_out,
           HealthReduction health_removed) {
  for (const integration::StatRequest& request : requests) {
    const char* verb = request.relative ? "add" : "set";
    if (!soul || knocked_out) {
      if (log_line) {
        log_line("STATS %s %u=%.3f dropped (no living body)", verb, unsigned(request.stat),
                 request.value);
      }
      continue;
    }
    if (request.stat >= integration::kWritableStateCount) {
      if (log_line) {
        log_line("STATS %s %u=%.3f dropped (not a writable state)", verb, unsigned(request.stat),
                 request.value);
      }
      continue;
    }
    // addStat: the change is added to what this game's state holds right now.
    const float value = request.relative ? changed_value(soul, request) : request.value;
    if (request.stat == kHealthState && value == 0.0f) {
      deal_lethal_damage(soul);
    } else {
      const bool report_health = health_removed && request.stat == kHealthState && value > 0.0f;
      const float health_before = report_health ? state(soul, kHealthState) : 0.0f;
      reinterpret_cast<SetState>(natives.set_state)(soul, request.stat, value);
      if (report_health) {
        const float health_after = state(soul, kHealthState);
        if (health_before > health_after) {
          health_removed(health_before - health_after);
        }
      }
    }
    if (log_line) {
      if (request.relative) {
        log_line("STATS add %s%+.3f -> %.3f", integration::kVitalNames[request.stat], request.value,
                 value);
      } else {
        log_line("STATS set %s=%.3f", integration::kVitalNames[request.stat], value);
      }
    }
  }
}

void keep_replica_vitals(void* soul, const integration::PlayerVitals& owner) {
  // The soul's state indices of stamina and exhaustion (kVitalNames order).
  constexpr unsigned kStaminaState = 1;
  constexpr unsigned kExhaustState = 2;
  // Closer than this the replica is left as it is: the owner publishes rounded values.
  constexpr float kSettleDifference = 1.0f;
  if (!soul || !natives.set_state) {
    return;
  }
  for (const unsigned index : {kHealthState, kStaminaState, kExhaustState}) {
    const float wanted = owner.values[index];
    // A knock-out comes with its own sync (Client/runtime/knockout): health 0 is never set here.
    if (index == kHealthState && wanted <= 0.0f) {
      continue;
    }
    // Health is authoritative to the precision carried on the wire. A one-unit deadband could
    // leave a low-health replica at a different lethal-hit threshold from its owner.
    const float current = state(soul, index);
    const bool changed = index == kHealthState ? integration::stat_field_value(current) != wanted
                                               : std::fabs(current - wanted) > kSettleDifference;
    if (changed) {
      reinterpret_cast<SetState>(natives.set_state)(soul, index, wanted);
    }
  }
}

}  // namespace kcds::runtime::stats
