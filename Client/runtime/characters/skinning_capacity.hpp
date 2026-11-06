// Apply GPU skinning budgets only while the engine naturally initializes a new stage.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::characters::skinning {

using Logger = void (*)(const char*, ...);

struct Status {
  bool installed = false;
  unsigned scale = 4;
  std::uint32_t initialized_stages = 0;
  std::uint32_t increased_stages = 0;
  std::uint32_t skipped_stages = 0;
  std::uint32_t last_bones = 0;
  std::uint32_t last_previous_bones = 0;
  std::uint32_t last_morphs = 0;
};

// Call synchronously after WHGame.dll finishes loading, before engine startup. MinHook is
// initialized here if needed. Repeated calls are harmless. Failure leaves native behavior intact.
// KCDS_SKINNING_CAPACITY_SCALE: absent=4, integer1..8; 1 preserves native/user configuration.
bool install(std::uint8_t* image, std::size_t image_size, Logger logger);

// A hook installed after an existing stage initialized does not resize it. Until an Init is
// observed, report_status explicitly describes the setting as pending, not applied.
Status status();
void report_status();

}  // namespace kcds::runtime::characters::skinning
