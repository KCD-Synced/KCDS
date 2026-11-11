#include "../feature_capacity_plan.hpp"

#include <initializer_list>
#include <limits>
#include <string>

#include "kcd2s/testkit.hpp"

using namespace kcds::runtime::characters::features;

KCD2S_TEST(feature_capacity, default_plan_is_whole_blocks) {
  KCD2S_CHECK_EQ(kNativeFeatures, 55296u);
  const auto plan = make_plan(kDefaultScale, kNativeFeatures);
  KCD2S_CHECK(plan.result == PlanResult::increase);
  KCD2S_CHECK_EQ(plan.effective, 442368u);
  KCD2S_CHECK_EQ(blocks(plan.effective), 16384u);
  KCD2S_CHECK_EQ(plan.effective % kBlockFeatures, 0u);
  KCD2S_CHECK_EQ(allocation_bytes(plan.effective), std::uint64_t{56623104});
}

KCD2S_TEST(feature_capacity, baseline_and_larger_configurations_are_kept) {
  for (const auto count : {54u, kNativeFeatures, kNativeFeatures * 20}) {
    const auto baseline = make_plan(1, count);
    KCD2S_CHECK(baseline.result == PlanResult::baseline);
    KCD2S_CHECK_EQ(baseline.effective, count);
  }
  const auto larger = make_plan(kDefaultScale, kNativeFeatures * 12);
  KCD2S_CHECK(larger.result == PlanResult::unchanged);
  KCD2S_CHECK_EQ(larger.effective, kNativeFeatures * 12);
  // A floor, not a repeated multiplication.
  KCD2S_CHECK_EQ(make_plan(8, make_plan(8, kNativeFeatures).effective).effective,
                 kNativeFeatures * 8);
}

KCD2S_TEST(feature_capacity, rejects_partial_blocks_and_byte_overflow) {
  for (const auto count :
       {0u, 27u, 28u, kNativeFeatures + 1, std::numeric_limits<std::uint32_t>::max()}) {
    KCD2S_CHECK(!make_plan(kDefaultScale, count).valid());
  }
  KCD2S_CHECK(valid_capacity(kNativeFeatures * kMaximumScale));
  KCD2S_CHECK(allocation_bytes(kNativeFeatures * kMaximumScale) + 255 <= UINT32_MAX);
}

KCD2S_TEST(feature_capacity, strict_environment_scale) {
  for (const auto value : {"", "0", "17", "-1", "+8", "8.0", "8x", " 8", "08", "999"}) {
    KCD2S_CHECK(!parse_scale(value));
  }
  for (unsigned scale = 1; scale <= kMaximumScale; ++scale) {
    const auto parsed = parse_scale(std::to_string(scale));
    KCD2S_CHECK_EQ(parsed.value_or(0), scale);
    KCD2S_CHECK(make_plan(scale, kNativeFeatures).valid());
  }
  KCD2S_CHECK(!make_plan(0, kNativeFeatures).valid());
  KCD2S_CHECK(!make_plan(kMaximumScale + 1, kNativeFeatures).valid());
}

KCD2S_TEST(feature_capacity, constructor_result_must_match_exactly) {
  const auto expected = kNativeFeatures * 8;
  const std::uintptr_t begin = 0x10000;
  const auto end = begin + static_cast<std::uintptr_t>(allocation_bytes(expected));
  KCD2S_CHECK(matches_cpu_allocation(expected, blocks(expected), 1, begin, end));
  // Native count, unreserved block zero, short vector and missing storage are all refused.
  KCD2S_CHECK(!matches_cpu_allocation(expected, 2048, 1, begin, end));
  KCD2S_CHECK(!matches_cpu_allocation(expected, blocks(expected), 0, begin, end));
  KCD2S_CHECK(!matches_cpu_allocation(expected, blocks(expected), 1, begin, end - kElementBytes));
  KCD2S_CHECK(!matches_cpu_allocation(expected, blocks(expected), 1, 0, end));
}
