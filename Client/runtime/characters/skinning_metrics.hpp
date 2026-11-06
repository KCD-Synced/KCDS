#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::characters::skinning::metrics {

using Logger = void (*)(const char*, ...);

// Test diagnostics only. Both KCDS_TEST_HOOKS=1 and KCDS_TEST_CHARACTER_METRICS=1
// are required. Otherwise no hooks or recurring work are installed. Call during startup.
// Reports accepted upload records, not character count, requested demand, or overflow.
bool install(std::uint8_t* image, std::size_t image_size, Logger logger);

}  // namespace kcds::runtime::characters::skinning::metrics
