#include "../material_capacity_plan.hpp"

#include <initializer_list>
#include <limits>
#include <string>

#include "kcd2s/testkit.hpp"

using namespace kcds::runtime::characters::materials;

KCD2S_TEST(material_capacity, default_budget_and_reserved_slot_are_bounded) {
  const auto plan = make_plan(kDefaultScale, kNativeCapacity);
  KCD2S_CHECK(plan.result == PlanResult::increase);
  KCD2S_CHECK_EQ(plan.effective, 64000u);
  KCD2S_CHECK_EQ(allocation_bytes(plan.effective), std::uint64_t{8192000});
  KCD2S_CHECK_EQ(plan.effective - 1, 63999u);
  KCD2S_CHECK(valid_capacity(plan.effective));
}

KCD2S_TEST(material_capacity, baseline_preserves_small_and_larger_user_configuration) {
  for (const auto count : {1u, 128u, kNativeCapacity, 32768u, kMaximumCapacity}) {
    const auto baseline = make_plan(1, count);
    KCD2S_CHECK(baseline.result == PlanResult::baseline);
    KCD2S_CHECK_EQ(baseline.effective, count);
  }
  for (const auto count : {64000u, kMaximumCapacity}) {
    const auto plan = make_plan(kDefaultScale, count);
    KCD2S_CHECK(plan.result == PlanResult::unchanged);
    KCD2S_CHECK_EQ(plan.effective, count);
  }
  // A floor, not repeated multiplication: repeated lifecycle construction cannot compound it.
  KCD2S_CHECK_EQ(make_plan(8, make_plan(8, 2560).effective).effective, 20480u);
}

KCD2S_TEST(material_capacity, rejects_truncating_ids_and_byte_overflow) {
  for (const auto count : {0u, 65536u, 0x10001u, std::numeric_limits<std::uint32_t>::max()}) {
    KCD2S_CHECK(!make_plan(8, count).valid());
    KCD2S_CHECK(!make_plan(1, count).valid());
  }
  KCD2S_CHECK(valid_capacity(65535));
  KCD2S_CHECK_EQ(allocation_bytes(65535), std::uint64_t{8388480});
  KCD2S_CHECK(allocation_bytes(65535) + 255 <= UINT32_MAX);
}

KCD2S_TEST(material_capacity, strict_environment_scale_rejects_ambiguous_values) {
  for (const auto value : {"", "0", "26", "-1", "+8", "8.0", "8x", " 8", "8 ", "08", "999999"}) {
    KCD2S_CHECK(!parse_scale(value));
  }
  for (unsigned scale = 1; scale <= kMaximumScale; ++scale) {
    const std::string text = std::to_string(scale);
    const auto parsed = parse_scale(text);
    KCD2S_CHECK(parsed.has_value());
    KCD2S_CHECK_EQ(parsed.value_or(0), scale);
    KCD2S_CHECK(make_plan(scale, kNativeCapacity).valid());
  }
  KCD2S_CHECK(!make_plan(0, kNativeCapacity).valid());
  KCD2S_CHECK(!make_plan(26, kNativeCapacity).valid());
}

KCD2S_TEST(material_capacity, allocation_verification_rejects_partial_and_wrong_resources) {
  const std::uintptr_t begin = 0x10000;
  const auto count = make_plan(8, kNativeCapacity).effective;
  const auto end = begin + allocation_bytes(count);
  KCD2S_CHECK(matches_cpu_allocation(count, 20480, begin, end, end));
  KCD2S_CHECK(!matches_cpu_allocation(count, 2560, begin, end, end));
  KCD2S_CHECK(!matches_cpu_allocation(count, 20480, begin, end - 128, end));
  KCD2S_CHECK(!matches_cpu_allocation(count, 20480, begin, end, end - 1));
  KCD2S_CHECK(!matches_cpu_allocation(count, 20480, 0, allocation_bytes(count), end));
  KCD2S_CHECK(!matches_cpu_allocation(count, 20480, end, begin, end));
  KCD2S_CHECK(matches_gpu_allocation(count, count, 128, 0x10000));
  KCD2S_CHECK(!matches_gpu_allocation(count, 2560, 128, 0x10000));
  KCD2S_CHECK(!matches_gpu_allocation(count, count, 64, 0x10000));
  KCD2S_CHECK(!matches_gpu_allocation(count, count, 128, 0));
}
