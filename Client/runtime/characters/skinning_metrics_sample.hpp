#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace kcds::runtime::characters::skinning::metrics {

constexpr std::uint32_t kReservedRecords = 768;
constexpr std::uint32_t kMaximumRequestSample = 65536;

struct BufferSample {
  std::uint32_t used = 0;
  std::uint32_t capacity = 0;
  bool valid = false;
};

inline BufferSample decode_buffer(std::uint32_t bytes, std::uint32_t capacity, std::uint32_t stride,
                                  std::uint32_t expected_stride) {
  if (stride != expected_stride || (stride != 8 && stride != 32) || bytes % stride != 0 ||
      capacity <= kReservedRecords ||
      capacity > std::numeric_limits<std::uint32_t>::max() / stride) {
    return {};
  }
  const auto used = bytes / stride;
  // The native branch accepts a request only when the resulting cursor is strictly below
  // capacity. The initial identity/zero records are included in the Unmap byte range.
  if (used < kReservedRecords || used >= capacity) {
    return {};
  }
  return {used, capacity, true};
}

struct UploadSample {
  BufferSample bones;
  BufferSample morphs;
  std::uintptr_t renderer_key = 0;
  std::uint32_t frame = 0;
  std::uint32_t requests = 0;
  bool frame_valid = false;
  bool requests_valid = false;
  bool duplicate = false;
  unsigned bone_calls = 0;
  unsigned morph_calls = 0;

  bool complete() const {
    return frame_valid && bones.valid && morphs.valid && !duplicate && bone_calls == 1 &&
           morph_calls == 1;
  }
};

struct Totals {
  std::uint64_t uploads = 0;
  std::uint64_t complete = 0;
  std::uint64_t incomplete = 0;
  std::uint64_t frames = 0;
  std::uint64_t frame_table_full = 0;
  std::uint64_t request_sample_missing = 0;
  std::uint32_t requests_sample_peak = 0;
  std::uint32_t accepted_bones_peak = 0;
  std::uint32_t accepted_morphs_peak = 0;
  std::uint32_t bone_used_peak = 0;
  std::uint32_t morph_used_peak = 0;
  std::uint32_t bone_capacity_min = 0;
  std::uint32_t bone_capacity_max = 0;
  std::uint32_t morph_capacity_min = 0;
  std::uint32_t morph_capacity_max = 0;
  std::uint32_t bone_utilization_ppm = 0;
  std::uint32_t morph_utilization_ppm = 0;
};

class Window {
 public:
  // This is a bounded scalar identity set, never a list of native object references to read.
  static constexpr std::size_t kFrameSlots = 2048;

  void add(const UploadSample& sample) {
    ++totals.uploads;
    if (!sample.complete()) {
      ++totals.incomplete;
      return;
    }
    ++totals.complete;
    count_frame(sample.renderer_key, sample.frame);
    if (sample.requests_valid && sample.requests <= kMaximumRequestSample) {
      totals.requests_sample_peak = std::max(totals.requests_sample_peak, sample.requests);
    } else {
      ++totals.request_sample_missing;
    }
    totals.accepted_bones_peak =
        std::max(totals.accepted_bones_peak, sample.bones.used - kReservedRecords);
    totals.accepted_morphs_peak =
        std::max(totals.accepted_morphs_peak, sample.morphs.used - kReservedRecords);
    totals.bone_used_peak = std::max(totals.bone_used_peak, sample.bones.used);
    totals.morph_used_peak = std::max(totals.morph_used_peak, sample.morphs.used);
    capacity_range(sample.bones.capacity, totals.bone_capacity_min, totals.bone_capacity_max);
    capacity_range(sample.morphs.capacity, totals.morph_capacity_min, totals.morph_capacity_max);
    totals.bone_utilization_ppm = std::max(totals.bone_utilization_ppm, utilization(sample.bones));
    totals.morph_utilization_ppm =
        std::max(totals.morph_utilization_ppm, utilization(sample.morphs));
  }

  void reset() {
    totals = {};
    frames.fill({});
  }

  Totals totals;

 private:
  struct FrameKey {
    std::uintptr_t renderer = 0;
    std::uint32_t frame = 0;
    bool occupied = false;
  };
  std::array<FrameKey, kFrameSlots> frames{};

  static void capacity_range(std::uint32_t value, std::uint32_t& minimum, std::uint32_t& maximum) {
    minimum = minimum ? std::min(minimum, value) : value;
    maximum = std::max(maximum, value);
  }

  static std::uint32_t utilization(const BufferSample& sample) {
    return static_cast<std::uint32_t>(std::uint64_t{sample.used} * 1000000 / sample.capacity);
  }

  void count_frame(std::uintptr_t renderer, std::uint32_t frame) {
    const auto hash = (renderer >> 4) ^ (std::uint64_t{frame} * 2654435761u);
    for (std::size_t probe = 0; probe < kFrameSlots; ++probe) {
      auto& key = frames[(hash + probe) % kFrameSlots];
      if (!key.occupied) {
        key = {renderer, frame, true};
        ++totals.frames;
        return;
      }
      if (key.renderer == renderer && key.frame == frame) {
        return;
      }
    }
    ++totals.frame_table_full;
  }
};

}  // namespace kcds::runtime::characters::skinning::metrics
