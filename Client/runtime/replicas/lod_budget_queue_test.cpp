// The game's actor LOD budget queue keeps only actors its update can take.
#include "lod_budget_queue.hpp"

#include <vector>

#include "kcd2s/testkit.hpp"

using kcds::runtime::replicas::keep_takeable;

namespace {

int actors[6];

void* actor(int index) {
  return &actors[index];
}

std::size_t keep(std::vector<void*>& queue, const std::vector<void*>& raise,
                 const std::vector<void*>& lower) {
  const std::size_t kept =
      keep_takeable(queue.data(), queue.size(), raise.data(), raise.data() + raise.size(),
                    lower.data(), lower.data() + lower.size());
  queue.resize(kept);
  return kept;
}

}  // namespace

KCD2S_TEST(lod_budget_queue, actors_in_either_list_stay_in_their_order) {
  std::vector<void*> queue{actor(0), actor(1), actor(2)};
  KCD2S_CHECK_EQ(keep(queue, {actor(2), actor(0)}, {actor(1)}), std::size_t(3));
  KCD2S_CHECK(queue == (std::vector<void*>{actor(0), actor(1), actor(2)}));
}

KCD2S_TEST(lod_budget_queue, an_actor_in_no_list_is_left_out) {
  std::vector<void*> queue{actor(0), actor(3), actor(1)};
  KCD2S_CHECK_EQ(keep(queue, {actor(0)}, {actor(1)}), std::size_t(2));
  KCD2S_CHECK(queue == (std::vector<void*>{actor(0), actor(1)}));
}

KCD2S_TEST(lod_budget_queue, an_actor_queued_twice_is_taken_once) {
  std::vector<void*> queue{actor(0), actor(1), actor(0), actor(1), actor(0)};
  KCD2S_CHECK_EQ(keep(queue, {actor(0)}, {actor(1)}), std::size_t(2));
  KCD2S_CHECK(queue == (std::vector<void*>{actor(0), actor(1)}));
}

KCD2S_TEST(lod_budget_queue, empty_lists_leave_nothing) {
  std::vector<void*> queue{actor(4), actor(5)};
  KCD2S_CHECK_EQ(keep(queue, {}, {}), std::size_t(0));
}
