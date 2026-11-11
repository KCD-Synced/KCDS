#include "player_stats.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../core/game_layouts.hpp"
#include "kcd2s/testkit.hpp"

// The knockout module is not under test: nobody is kept down here.
namespace kcds::runtime::knockout {
bool kept_down_now() {
  return false;
}
}  // namespace kcds::runtime::knockout

namespace {

namespace integration = kcd2s::integration;
namespace stats = kcds::runtime::stats;

constexpr std::size_t kSoulStates = 1800;
constexpr std::size_t kSoulDamage = kcds::layouts::Soul::kDamage;

struct AppliedState {
  unsigned state = 0;
  float value = 0;
};

enum class EventKind {
  StateSet,
  NativeDamage,
  HealthReduction,
};

struct Event {
  EventKind kind;
  float value;
};

std::vector<AppliedState> applied;
std::vector<Event> events;
std::vector<float> reductions;
std::array<float, 5> conditions{};
std::array<float, 4> state_maxima{};

void __fastcall deal_damage(void* damage, float stamina, float health, void* attacker, char flag,
                            void* type);

struct Soul {
  alignas(16) std::array<std::uint8_t, 2048> bytes{};
  std::array<void*, 28> damage_vtable{};

  Soul() {
    damage_vtable[27] = reinterpret_cast<void*>(deal_damage);
    void** table = damage_vtable.data();
    std::memcpy(bytes.data() + kSoulDamage, &table, sizeof(table));
  }

  void set(unsigned state, float value) {
    std::memcpy(bytes.data() + kSoulStates + state * sizeof(value), &value, sizeof(value));
  }

  void* data() {
    return bytes.data();
  }
};

float __fastcall derived(void*, int id, void*) {
  constexpr std::array<int, 5> ids{35, 54, 55, 63, 64};
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (ids[index] == id) {
      return conditions[index];
    }
  }
  return 0;
}

float __fastcall level(void*, int, void*) {
  return 1;
}

float __fastcall maximum(void*, unsigned state) {
  return state_maxima[state];
}

void __fastcall set_state(void* soul, unsigned state, float value) {
  applied.push_back({state, value});
  const float clamped = std::clamp(value, 0.0f, state_maxima[state]);
  events.push_back({EventKind::StateSet, clamped});
  std::memcpy(static_cast<std::uint8_t*>(soul) + kSoulStates + state * sizeof(clamped), &clamped,
              sizeof(clamped));
}

void __fastcall deal_damage(void* damage, float, float health, void*, char, void*) {
  events.push_back({EventKind::NativeDamage, health});
  auto states = static_cast<std::uint8_t*>(damage) - kSoulDamage + kSoulStates;
  float before = 0;
  std::memcpy(&before, states, sizeof(before));
  const float after = std::max(0.0f, before - health);
  std::memcpy(states, &after, sizeof(after));
}

void health_removed(float amount) {
  reductions.push_back(amount);
  events.push_back({EventKind::HealthReduction, amount});
}

const void* __fastcall parameters() {
  static const std::array<std::uint8_t, 256> bytes{};
  return bytes.data();
}

void* __fastcall initialize_type(void* type) {
  return type;
}

void __fastcall free_type(void*) {
}

void initialize(kcd2s::testkit::Context& kcd2s_ctx) {
  applied.clear();
  events.clear();
  reductions.clear();
  conditions = {0, 0, 1, 0, 0};
  state_maxima.fill(100);
  stats::Natives natives;
  natives.derived_stat = reinterpret_cast<void*>(derived);
  natives.stat_level_relative = reinterpret_cast<void*>(level);
  natives.set_state = reinterpret_cast<void*>(set_state);
  natives.state_max = reinterpret_cast<void*>(maximum);
  natives.stat_level = reinterpret_cast<void*>(level);
  natives.skill_level = reinterpret_cast<void*>(level);
  natives.rpg_params = reinterpret_cast<void*>(parameters);
  natives.damage_type_init = reinterpret_cast<void*>(initialize_type);
  natives.damage_type_free = reinterpret_cast<void*>(free_type);
  KCD2S_CHECK(stats::install(natives, nullptr));
  stats::capture(nullptr, 0);
}

integration::PlayerVitals read_published(kcd2s::testkit::Context& kcd2s_ctx, Soul& soul,
                                         std::uint64_t now) {
  integration::PlayerVitals vitals;
  KCD2S_CHECK(integration::parse_vitals_field(stats::capture(soul.data(), now).vitals, vitals));
  return vitals;
}

}  // namespace

KCD2S_TEST(runtime_player_stats, health_changes_publish_before_the_state_period) {
  initialize(kcd2s_ctx);
  Soul soul;
  soul.set(0, 100);
  soul.set(1, 100);
  KCD2S_CHECK(read_published(kcd2s_ctx, soul, 1000).values[0] == 100);

  soul.set(0, 99.99f);
  KCD2S_CHECK(read_published(kcd2s_ctx, soul, 1001).values[0] == 99.99f);
  soul.set(0, 1);
  KCD2S_CHECK(read_published(kcd2s_ctx, soul, 1002).values[0] == 1);
  soul.set(0, 100);
  KCD2S_CHECK(read_published(kcd2s_ctx, soul, 1003).values[0] == 100);
}

KCD2S_TEST(runtime_player_stats, non_health_states_remain_throttled_and_conditions_immediate) {
  initialize(kcd2s_ctx);
  Soul soul;
  soul.set(0, 100);
  soul.set(1, 100);
  KCD2S_CHECK(read_published(kcd2s_ctx, soul, 2000).values[1] == 100);

  soul.set(1, 80);
  KCD2S_CHECK(read_published(kcd2s_ctx, soul, 2099).values[1] == 100);
  KCD2S_CHECK(read_published(kcd2s_ctx, soul, 2100).values[1] == 80);
  conditions[2] = 0;
  KCD2S_CHECK(read_published(kcd2s_ctx, soul, 2101).values[6] == 0);
}

KCD2S_TEST(runtime_player_stats, replica_health_reconciles_below_one_unit) {
  initialize(kcd2s_ctx);
  Soul soul;
  soul.set(0, 1);
  soul.set(1, 90);
  soul.set(2, 80);
  integration::PlayerVitals owner;
  owner.values[0] = 1.01f;
  owner.values[1] = 90.5f;
  owner.values[2] = 80.5f;
  stats::keep_replica_vitals(soul.data(), owner);
  KCD2S_CHECK_EQ(applied.size(), std::size_t(1));
  KCD2S_CHECK(applied.size() == 1 && applied[0].state == 0 && applied[0].value == 1.01f);

  // A replica's native float can differ within the same encoded hundredth without a write.
  applied.clear();
  soul.set(0, 1.0101f);
  stats::keep_replica_vitals(soul.data(), owner);
  KCD2S_CHECK(applied.empty());
}

KCD2S_TEST(runtime_player_stats, zero_health_is_left_to_knockout_and_positive_owner_repairs_zero) {
  initialize(kcd2s_ctx);
  Soul soul;
  soul.set(0, 1);
  soul.set(1, 100);
  soul.set(2, 100);
  integration::PlayerVitals owner;
  owner.values[0] = 0;
  owner.values[1] = 100;
  owner.values[2] = 100;
  stats::keep_replica_vitals(soul.data(), owner);
  KCD2S_CHECK(applied.empty());

  soul.set(0, 0);
  owner.values[0] = 0.01f;
  stats::keep_replica_vitals(soul.data(), owner);
  KCD2S_CHECK_EQ(applied.size(), std::size_t(1));
  KCD2S_CHECK(applied.size() == 1 && applied[0].state == 0 && applied[0].value == 0.01f);
}

KCD2S_TEST(runtime_player_stats, scripted_health_reports_the_actual_native_clamp) {
  initialize(kcd2s_ctx);
  Soul soul;
  soul.set(0, 120);

  // The body's maximum has fallen below its current cache. Requesting 110 applies 100,
  // so the actual reduction is 20, rather than the 10 suggested by the request.
  stats::apply(soul.data(), {{0, 110}, {0, 150}, {0, 90}}, false, health_removed);

  KCD2S_CHECK_EQ(reductions.size(), std::size_t(2));
  KCD2S_CHECK(reductions.size() == 2 && reductions[0] == 20 && reductions[1] == 10);
  KCD2S_CHECK_EQ(applied.size(), std::size_t(3));
}

KCD2S_TEST(runtime_player_stats, healing_noops_and_other_states_do_not_report_health_damage) {
  initialize(kcd2s_ctx);
  Soul soul;
  soul.set(0, 50);
  soul.set(1, 100);
  soul.set(2, 100);
  soul.set(3, 100);

  stats::apply(soul.data(), {{0, 70}, {0, 70}, {1, 20}, {2, 30}, {3, 40}, {0, 1000000}}, false,
               health_removed);

  KCD2S_CHECK(reductions.empty());
  KCD2S_CHECK_EQ(applied.size(), std::size_t(6));
}

KCD2S_TEST(runtime_player_stats, missing_and_knocked_out_bodies_drop_health_reports) {
  initialize(kcd2s_ctx);
  Soul soul;
  soul.set(0, 100);

  const std::vector<integration::StatRequest> requests{{0, 10}, {0, 0}, {1, 20}};
  stats::apply(soul.data(), requests, true, health_removed);
  stats::apply(nullptr, requests, false, health_removed);

  KCD2S_CHECK(applied.empty());
  KCD2S_CHECK(reductions.empty());
  KCD2S_CHECK(events.empty());
}

KCD2S_TEST(runtime_player_stats, positive_setter_reports_stay_ordered_before_native_zero_damage) {
  initialize(kcd2s_ctx);
  Soul soul;
  soul.set(0, 100);

  stats::apply(soul.data(), {{0, 80}, {1, 80}, {0, 50}, {0, 0}}, false, health_removed);

  KCD2S_CHECK_EQ(reductions.size(), std::size_t(2));
  KCD2S_CHECK(reductions.size() == 2 && reductions[0] == 20 && reductions[1] == 30);
  KCD2S_CHECK_EQ(events.size(), std::size_t(6));
  if (events.size() == 6) {
    KCD2S_CHECK(events[0].kind == EventKind::StateSet && events[0].value == 80);
    KCD2S_CHECK(events[1].kind == EventKind::HealthReduction && events[1].value == 20);
    KCD2S_CHECK(events[2].kind == EventKind::StateSet && events[2].value == 80);
    KCD2S_CHECK(events[3].kind == EventKind::StateSet && events[3].value == 50);
    KCD2S_CHECK(events[4].kind == EventKind::HealthReduction && events[4].value == 30);
    KCD2S_CHECK(events[5].kind == EventKind::NativeDamage && events[5].value == 51);
  }
}

KCD2S_TEST(runtime_player_stats, add_stat_changes_the_current_value_within_its_range) {
  initialize(kcd2s_ctx);
  state_maxima = {100, 120, 100, 150};
  Soul soul;
  soul.set(0, 100);
  soul.set(1, 60);
  soul.set(2, 55);
  soul.set(3, 140);

  stats::apply(soul.data(),
               {{2, 25, true}, {2, 30, true}, {3, -40.5f, true}, {3, -1000000, true}, {1, 0, true}},
               false, health_removed);

  // Each change starts from what the state holds after the one before it, kept within 0 and the
  // state's own maximum before the setter sees it.
  KCD2S_CHECK_EQ(applied.size(), std::size_t(5));
  if (applied.size() == 5) {
    KCD2S_CHECK(applied[0].state == 2 && applied[0].value == 80);
    KCD2S_CHECK(applied[1].state == 2 && applied[1].value == 100);
    KCD2S_CHECK(applied[2].state == 3 && applied[2].value == 99.5f);
    KCD2S_CHECK(applied[3].state == 3 && applied[3].value == 0);
    KCD2S_CHECK(applied[4].state == 1 && applied[4].value == 60);
  }
  KCD2S_CHECK(reductions.empty());
}

KCD2S_TEST(runtime_player_stats, add_stat_lowering_health_reports_and_zero_is_native_damage) {
  initialize(kcd2s_ctx);
  Soul soul;
  soul.set(0, 80);

  stats::apply(soul.data(), {{0, -30, true}, {0, 500, true}, {0, -100.000001f, true}}, false,
               health_removed);

  KCD2S_CHECK_EQ(reductions.size(), std::size_t(1));
  KCD2S_CHECK(reductions.size() == 1 && reductions[0] == 30);
  KCD2S_CHECK_EQ(events.size(), std::size_t(4));
  if (events.size() == 4) {
    KCD2S_CHECK(events[0].kind == EventKind::StateSet && events[0].value == 50);
    KCD2S_CHECK(events[1].kind == EventKind::HealthReduction && events[1].value == 30);
    KCD2S_CHECK(events[2].kind == EventKind::StateSet && events[2].value == 100);
    // Health brought to 0 is the game's own lethal damage, as setStat('health', 0).
    KCD2S_CHECK(events[3].kind == EventKind::NativeDamage && events[3].value == 101);
  }
}

KCD2S_TEST(runtime_player_stats, add_stat_to_a_knocked_out_body_is_dropped) {
  initialize(kcd2s_ctx);
  Soul soul;
  soul.set(3, 50);

  stats::apply(soul.data(), {{3, 10, true}}, true, health_removed);
  stats::apply(nullptr, {{3, 10, true}}, false, health_removed);

  KCD2S_CHECK(applied.empty());
  KCD2S_CHECK(events.empty());
}

KCD2S_TEST(runtime_player_stats, other_callers_can_leave_health_reporting_disabled) {
  initialize(kcd2s_ctx);
  Soul soul;
  soul.set(0, 100);

  stats::apply(soul.data(), {{0, 25}}, false);

  KCD2S_CHECK_EQ(applied.size(), std::size_t(1));
  KCD2S_CHECK(reductions.empty());
  KCD2S_CHECK_EQ(events.size(), std::size_t(1));
  KCD2S_CHECK(events.size() == 1 && events[0].kind == EventKind::StateSet);
}
