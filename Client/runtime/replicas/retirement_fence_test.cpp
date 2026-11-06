#include "retirement_fence.hpp"

#include <array>
#include <unordered_map>

#include "kcd2s/testkit.hpp"

using kcds::runtime::replicas::RetirementFence;

KCD2S_TEST(retirement_fence, delayed_retirement_reserves_only_its_own_allocation) {
  RetirementFence fence;
  int player, npc, horse, replacement, extra;
  std::unordered_map<unsigned, const void*> live{{1, &player}, {2, &npc}, {3, &horse}};
  const auto lookup = [&live](unsigned id) {
    const auto found = live.find(id);
    return found == live.end() ? nullptr : found->second;
  };
  for (const auto& [id, body] : live) {
    fence.created(id, body);
  }
  KCD2S_CHECK(!fence.can_spawn(3));
  fence.retire(3, &horse);
  fence.refresh(lookup);
  KCD2S_CHECK(fence.active() == 2);
  KCD2S_CHECK(fence.pending() == 1);
  KCD2S_CHECK(!fence.can_spawn(3));
  // Growing the admitted view has a free slot despite the deferred horse.
  KCD2S_CHECK(fence.can_spawn(4));
  live[4] = &extra;
  fence.created(4, &extra);
  KCD2S_CHECK(!fence.can_spawn(4));
  live.erase(3);
  fence.refresh(lookup);
  KCD2S_CHECK(fence.can_spawn(4));
  live[5] = &replacement;
  fence.created(5, &replacement);
  KCD2S_CHECK(!fence.can_spawn(4));
  KCD2S_CHECK(fence.pending() == 0);
  // A smaller view never grants additional allocations until old bodies leave.
  KCD2S_CHECK(!fence.can_spawn(2));
}

KCD2S_TEST(retirement_fence, dense_view_replacement_never_exceeds_native_allocation_count) {
  RetirementFence fence;
  std::array<int, 128> bodies{};
  std::unordered_map<unsigned, const void*> live;
  const auto lookup = [&live](unsigned id) {
    const auto found = live.find(id);
    return found == live.end() ? nullptr : found->second;
  };
  for (unsigned id = 1; id <= 64; ++id) {
    KCD2S_CHECK(fence.can_spawn(64));
    live[id] = &bodies[id - 1];
    fence.created(id, live[id]);
  }
  for (unsigned id = 1; id <= 64; ++id) {
    fence.retire(id, live[id]);
  }
  // All network departures may be acknowledged while all native actors still exist.
  fence.refresh(lookup);
  KCD2S_CHECK(!fence.can_spawn(64));
  for (unsigned id = 1; id <= 64; ++id) {
    live.erase(id);
    fence.refresh(lookup);
    KCD2S_CHECK(fence.can_spawn(64));
    const unsigned replacement = id + 64;
    live[replacement] = &bodies[replacement - 1];
    fence.created(replacement, live[replacement]);
    KCD2S_CHECK(!fence.can_spawn(64));
    KCD2S_CHECK(live.size() == 64);
  }
  KCD2S_CHECK(fence.active() == 64);
  KCD2S_CHECK(fence.pending() == 0);
}

KCD2S_TEST(retirement_fence, reused_id_and_system_replacement_do_not_hold_old_bodies) {
  RetirementFence fence;
  int old_body, new_body, old_system, new_system;
  fence.observe_system(&old_system);
  fence.created(8, &old_body);
  fence.retire(8, &old_body);
  fence.retire(8, &old_body);
  fence.created(8, &old_body);
  KCD2S_CHECK(fence.pending() == 1);
  KCD2S_CHECK(fence.active() == 0);
  fence.created(8, &new_body);
  fence.retire(8, &old_body);
  KCD2S_CHECK(fence.pending() == 0);
  KCD2S_CHECK(fence.active() == 1);
  KCD2S_CHECK(!fence.can_spawn(1));
  fence.observe_system(&new_system);
  KCD2S_CHECK(fence.can_spawn(1));
  fence.retire(0, &old_body);
  fence.retire(8, nullptr);
  fence.created(0, &old_body);
  fence.created(8, nullptr);
  KCD2S_CHECK(fence.active() == 0);
  KCD2S_CHECK(fence.pending() == 0);
}

KCD2S_TEST(retirement_fence, same_system_reconnect_keeps_active_and_deferred_native_bodies) {
  RetirementFence fence;
  int system, active, retiring;
  fence.observe_system(&system);
  fence.created(8, &active);
  fence.created(9, &retiring);
  fence.retire(9, &retiring);
  // Forgetting a session's maps does not prove native destruction or free either slot.
  fence.observe_system(nullptr);
  fence.observe_system(&system);
  fence.refresh([&](unsigned id) { return id == 8 ? &active : &retiring; });
  KCD2S_CHECK(!fence.can_spawn(2));
  KCD2S_CHECK(fence.active() == 1);
  KCD2S_CHECK(fence.pending() == 1);
  fence.refresh([](unsigned) -> const void* { return nullptr; });
  KCD2S_CHECK(fence.can_spawn(2));
  KCD2S_CHECK(fence.active() == 0);
  KCD2S_CHECK(fence.pending() == 0);
}
