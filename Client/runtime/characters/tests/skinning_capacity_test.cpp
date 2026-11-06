#include "../skinning_capacity_plan.hpp"

#include <initializer_list>
#include <limits>

#include "kcd2s/testkit.hpp"

using namespace kcds::runtime::characters::skinning;

KCD2S_TEST(skinning_capacity, explicit_baseline_preserves_native_and_user_budgets) {
  const auto plan = make_plan(1, 8192, 2048);
  KCD2S_CHECK(plan.result == PlanResult::baseline);
  KCD2S_CHECK_EQ(plan.effective.bones, 8192u);
  KCD2S_CHECK_EQ(plan.effective.morphs, 2048u);
}

KCD2S_TEST(skinning_capacity, default_growth_and_allocation_cost) {
  const auto plan = make_plan(kDefaultScale, kNativeBones, kNativeMorphs);
  KCD2S_CHECK(plan.result == PlanResult::increase);
  KCD2S_CHECK_EQ(plan.effective.bones, 524288u);
  KCD2S_CHECK_EQ(plan.effective.morphs, 16384u);
  KCD2S_CHECK_EQ(allocation_bytes(plan.effective), std::uint64_t{33685504});
}

KCD2S_TEST(skinning_capacity, preserves_larger_native_values_independently) {
  const auto plan = make_plan(4, 1048576, 8192);
  KCD2S_CHECK(plan.result == PlanResult::increase);
  KCD2S_CHECK_EQ(plan.effective.bones, 1048576u);
  KCD2S_CHECK_EQ(plan.effective.morphs, 16384u);
  const auto larger = make_plan(4, 1048576, 32768);
  KCD2S_CHECK(larger.result == PlanResult::unchanged);
  KCD2S_CHECK_EQ(larger.effective.morphs, 32768u);
}

KCD2S_TEST(skinning_capacity, rejects_unregistered_corrupt_and_imprecise_counts) {
  for (const auto count : {0, -1, 768, std::numeric_limits<std::int32_t>::max(),
                           static_cast<std::int32_t>(kMaximumElements + 1)}) {
    KCD2S_CHECK(!make_plan(4, count, kNativeMorphs).valid());
    KCD2S_CHECK(!make_plan(4, kNativeBones, count).valid());
  }
  KCD2S_CHECK(make_plan(4, kMaximumElements, kMaximumElements).valid());
}

KCD2S_TEST(skinning_capacity, scale_is_strict_and_bounded) {
  for (const auto value : {"", "0", "9", "-1", "1.5", "4x", " 4", "4 ", "04", "999999999"}) {
    KCD2S_CHECK(!parse_scale(value).has_value());
  }
  for (unsigned scale = 1; scale <= 8; ++scale) {
    const char digit = static_cast<char>('0' + scale);
    const auto parsed = parse_scale(std::string_view(&digit, 1));
    KCD2S_CHECK(parsed.has_value());
    KCD2S_CHECK_EQ(parsed.value_or(0), scale);
    KCD2S_CHECK(make_plan(scale, kNativeBones, kNativeMorphs).valid());
  }
  KCD2S_CHECK(!make_plan(0, kNativeBones, kNativeMorphs).valid());
  KCD2S_CHECK(!make_plan(9, kNativeBones, kNativeMorphs).valid());
  KCD2S_CHECK_EQ(make_plan(8, kNativeBones, kNativeMorphs).effective.bones, 1048576u);
}
