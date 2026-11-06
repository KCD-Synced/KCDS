// Increase only freshly constructed native clothing material pools; never resize live pools.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::characters::materials {

using Logger = void (*)(const char*, ...);

// Install synchronously after WHGame.dll loads, before native C3DEngine construction.
// KCDS_MATERIAL_CAPACITY_SCALE: absent=8, integer 1..8; 1 preserves native configuration.
// Larger valid configured capacities are preserved. A late installation cannot change an
// existing pool. All signature/ownership mismatches leave the native constructor unchanged.
bool install(std::uint8_t* image, std::size_t image_size, Logger logger);
void report_status();

}  // namespace kcds::runtime::characters::materials
