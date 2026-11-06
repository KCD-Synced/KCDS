#include "combat_mode.hpp"

#include <array>
#include <cstddef>

#include "kcd2s/testkit.hpp"

namespace {

using namespace kcds::runtime::npcs;

struct Core {
  std::uint8_t mode = 0;
  unsigned calls = 0;
};

struct Combat {
  CombatModeQuery* methods = nullptr;
  std::array<std::byte, 520> padding{};
  Core* core = nullptr;
};

struct Actor {
  std::array<std::byte, 632> padding{};
  Combat* combat = nullptr;
};

static_assert(offsetof(Actor, combat) == 632);
static_assert(offsetof(Combat, core) == 528);

std::uint8_t query_mode(void* object) {
  auto& core = *static_cast<Combat*>(object)->core;
  ++core.calls;
  return core.mode;
}

std::uint8_t unrelated_query(void* object) {
  ++static_cast<Combat*>(object)->core->calls;
  return 0;
}

struct Fixture {
  std::array<CombatModeQuery, 2> methods{nullptr, query_mode};
  Core core;
  Combat combat{methods.data(), {}, &core};
  Actor actor{{}, &combat};
};

}  // namespace

KCD2S_TEST(npc_combat_mode, existing_component_can_be_idle_and_tracks_live_mode_changes) {
  Fixture fixture;
  auto observed = observe_combat_mode(&fixture.actor, query_mode);
  KCD2S_CHECK(observed.component_present);
  KCD2S_CHECK(observed.mode == CombatMode::Idle);
  KCD2S_CHECK(!combat_needs_fast_sync(observed));
  fixture.core.mode = 1;
  observed = observe_combat_mode(&fixture.actor, query_mode);
  KCD2S_CHECK(observed.mode == CombatMode::Active);
  KCD2S_CHECK(combat_needs_fast_sync(observed));
  fixture.core.mode = 0;
  observed = observe_combat_mode(&fixture.actor, query_mode);
  KCD2S_CHECK(observed.mode == CombatMode::Idle);
  KCD2S_CHECK(fixture.core.calls == 3);
}

KCD2S_TEST(npc_combat_mode, absent_actor_or_component_remains_idle_without_native_calls) {
  KCD2S_CHECK(!combat_needs_fast_sync(observe_combat_mode(nullptr, query_mode)));
  Actor actor;
  const auto observed = observe_combat_mode(&actor, query_mode);
  KCD2S_CHECK(!observed.component_present);
  KCD2S_CHECK(observed.mode == CombatMode::Idle);
}

KCD2S_TEST(npc_combat_mode, unresolved_unknown_or_incomplete_native_keeps_fast_sync) {
  Fixture fixture;
  auto observed = observe_combat_mode(&fixture.actor, nullptr);
  KCD2S_CHECK(observed.component_present);
  KCD2S_CHECK(observed.mode == CombatMode::Unknown);
  KCD2S_CHECK(combat_needs_fast_sync(observed));
  fixture.methods[1] = unrelated_query;
  observed = observe_combat_mode(&fixture.actor, query_mode);
  KCD2S_CHECK(observed.mode == CombatMode::Unknown);
  KCD2S_CHECK(combat_needs_fast_sync(observed));
  fixture.combat.methods = nullptr;
  KCD2S_CHECK(combat_needs_fast_sync(observe_combat_mode(&fixture.actor, query_mode)));
  fixture.combat.methods = fixture.methods.data();
  fixture.methods[1] = query_mode;
  fixture.combat.core = nullptr;
  KCD2S_CHECK(combat_needs_fast_sync(observe_combat_mode(&fixture.actor, query_mode)));
  KCD2S_CHECK(fixture.core.calls == 0);
}
