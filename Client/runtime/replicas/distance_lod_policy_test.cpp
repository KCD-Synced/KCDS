#include "distance_lod_policy.hpp"

#include <limits>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcds::runtime::replicas::DistanceLodPolicy;

KCD2S_TEST(distance_lod, distance_hysteresis_restores_nearby_and_active_characters) {
  DistanceLodPolicy policy;
  int node;
  KCD2S_CHECK(!policy.update(&node, 100, 79 * 79, true));
  KCD2S_CHECK(policy.update(&node, 100, 80 * 80, true) == 108);
  KCD2S_CHECK(!policy.update(&node, 108, 70 * 70, true));
  KCD2S_CHECK(policy.update(&node, 108, 60 * 60, true) == 100);
  KCD2S_CHECK(!policy.update(&node, 100, 70 * 70, true));
  KCD2S_CHECK(policy.update(&node, 100, 90 * 90, true) == 108);
  KCD2S_CHECK(policy.update(&node, 108, 90 * 90, false) == 100);
  KCD2S_CHECK(!policy.update(&node, 100, 90 * 90, false));
}

KCD2S_TEST(distance_lod, queued_apply_and_restore_preserve_order_without_repeated_writes) {
  DistanceLodPolicy policy;
  int node;
  int native_ratio = 100;
  std::vector<int> queue;
  auto request = [&](bool idle) {
    if (const auto ratio = policy.update(&node, native_ratio, 100 * 100, idle)) {
      queue.push_back(*ratio);
    }
  };
  request(true);
  for (unsigned frame = 0; frame < 100; ++frame) {
    request(true);
  }
  KCD2S_CHECK(queue == std::vector<int>{108});
  // Combat begins while the first write still waits in the engine's queue.
  request(false);
  KCD2S_CHECK(queue == std::vector<int>({108, 100}));
  native_ratio = queue.front();
  request(false);
  KCD2S_CHECK(queue.size() == 2);
  native_ratio = queue.back();
  request(false);
  KCD2S_CHECK(native_ratio == 100);
  KCD2S_CHECK(queue.size() == 2);
  request(true);
  KCD2S_CHECK(queue == std::vector<int>({108, 100, 108}));
}

KCD2S_TEST(distance_lod, existing_and_observed_external_overrides_are_preserved) {
  DistanceLodPolicy policy;
  int node, replacement;
  KCD2S_CHECK(!policy.update(&node, 116, 100 * 100, true));
  KCD2S_CHECK(policy.update(&node, 100, 100 * 100, true) == 108);
  KCD2S_CHECK(!policy.update(&node, 108, 100 * 100, true));
  KCD2S_CHECK(!policy.update(&node, 116, 100 * 100, true));
  KCD2S_CHECK(!policy.update(&node, 116, 10, false));
  KCD2S_CHECK(!policy.update(&node, 100, 100 * 100, true));
  // A different render node is a new lifetime, not an external writer to the old one.
  KCD2S_CHECK(policy.update(&replacement, 100, 100 * 100, true) == 108);
  KCD2S_CHECK(!policy.update(&replacement, 108, 100 * 100, true));
  KCD2S_CHECK(!policy.update(&replacement, 100, 100 * 100, true));
  KCD2S_CHECK(!policy.update(&replacement, 100, 100 * 100, true));
}

KCD2S_TEST(distance_lod, invalid_distance_or_departure_cannot_leave_a_new_override) {
  DistanceLodPolicy policy;
  int node;
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(!policy.update(&node, 100, nan, true));
  KCD2S_CHECK(!policy.update(&node, 100, -1, true));
  KCD2S_CHECK(policy.update(&node, 100, 100 * 100, true) == 108);
  KCD2S_CHECK(policy.update(&node, 108, nan, true) == 100);
  KCD2S_CHECK(!policy.update(nullptr, 0, 0, false));
  KCD2S_CHECK(policy.update(&node, 100, 100 * 100, true) == 108);
}
