#include "../skinning_metrics_sample.hpp"

#include <limits>

#include "kcd2s/testkit.hpp"

using namespace kcds::runtime::characters::skinning::metrics;

namespace {

UploadSample sample(std::uint32_t frame = 1) {
  UploadSample value;
  value.bones = decode_buffer((kReservedRecords + 1000) * 32, 131072, 32, 32);
  value.morphs = decode_buffer((kReservedRecords + 20) * 8, 4096, 8, 8);
  value.renderer_key = 1;
  value.frame = frame;
  value.frame_valid = true;
  value.requests = 12;
  value.requests_valid = true;
  value.bone_calls = 1;
  value.morph_calls = 1;
  return value;
}

}  // namespace

KCD2S_TEST(skinning_metrics, unmap_ranges_include_reserved_records_and_strict_capacity) {
  const auto empty = decode_buffer(kReservedRecords * 32, 131072, 32, 32);
  KCD2S_CHECK(empty.valid);
  KCD2S_CHECK_EQ(empty.used, kReservedRecords);
  KCD2S_CHECK(decode_buffer((131072 - 1) * 32, 131072, 32, 32).valid);
  KCD2S_CHECK(!decode_buffer(131072 * 32, 131072, 32, 32).valid);
  KCD2S_CHECK(!decode_buffer((kReservedRecords - 1) * 32, 131072, 32, 32).valid);
  KCD2S_CHECK(!decode_buffer(kReservedRecords * 32 + 1, 131072, 32, 32).valid);
  KCD2S_CHECK(!decode_buffer(kReservedRecords * 32, 131072, 32, 8).valid);
  KCD2S_CHECK(!decode_buffer(kReservedRecords * 32, 131072, 0, 0).valid);
  KCD2S_CHECK(
      !decode_buffer(kReservedRecords * 32, std::numeric_limits<std::uint32_t>::max(), 32, 32)
           .valid);
}

KCD2S_TEST(skinning_metrics, accepted_peaks_exclude_reserve_and_sample_requests_are_separate) {
  Window window;
  auto value = sample();
  window.add(value);
  value.requests = kMaximumRequestSample + 1;
  value.bones = decode_buffer((kReservedRecords + 2000) * 32, 524288, 32, 32);
  window.add(value);
  KCD2S_CHECK_EQ(window.totals.complete, 2u);
  KCD2S_CHECK_EQ(window.totals.frames, 1u);
  KCD2S_CHECK_EQ(window.totals.accepted_bones_peak, 2000u);
  KCD2S_CHECK_EQ(window.totals.accepted_morphs_peak, 20u);
  KCD2S_CHECK_EQ(window.totals.bone_used_peak, kReservedRecords + 2000);
  KCD2S_CHECK_EQ(window.totals.requests_sample_peak, 12u);
  KCD2S_CHECK_EQ(window.totals.request_sample_missing, 1u);
  KCD2S_CHECK_EQ(window.totals.bone_capacity_min, 131072u);
  KCD2S_CHECK_EQ(window.totals.bone_capacity_max, 524288u);
  // Utilization is calculated per observation, not peak used divided by a different capacity.
  KCD2S_CHECK_EQ(window.totals.bone_utilization_ppm,
                 static_cast<std::uint32_t>(std::uint64_t{1768} * 1000000 / 131072));
}

KCD2S_TEST(skinning_metrics, incomplete_or_duplicate_pairs_do_not_pollute_peaks) {
  Window window;
  auto value = sample();
  value.morph_calls = 0;
  window.add(value);
  value = sample();
  value.duplicate = true;
  window.add(value);
  value = sample();
  value.frame_valid = false;
  window.add(value);
  value = sample();
  value.bones.valid = false;
  window.add(value);
  KCD2S_CHECK_EQ(window.totals.uploads, 4u);
  KCD2S_CHECK_EQ(window.totals.incomplete, 4u);
  KCD2S_CHECK_EQ(window.totals.complete, 0u);
  KCD2S_CHECK_EQ(window.totals.frames, 0u);
  KCD2S_CHECK_EQ(window.totals.accepted_bones_peak, 0u);
}

KCD2S_TEST(skinning_metrics, frame_count_is_deduplicated_bounded_and_resets_with_window) {
  Window window;
  for (std::uint32_t frame = 0; frame < Window::kFrameSlots; ++frame) {
    window.add(sample(frame));
    window.add(sample(frame));
  }
  window.add(sample(static_cast<std::uint32_t>(Window::kFrameSlots)));
  KCD2S_CHECK_EQ(window.totals.frames, Window::kFrameSlots);
  KCD2S_CHECK_EQ(window.totals.frame_table_full, 1u);
  window.reset();
  window.add(sample());
  auto second_renderer = sample();
  second_renderer.renderer_key = 2;
  window.add(second_renderer);
  KCD2S_CHECK_EQ(window.totals.uploads, 2u);
  KCD2S_CHECK_EQ(window.totals.frames, 2u);
  KCD2S_CHECK_EQ(window.totals.frame_table_full, 0u);
}
