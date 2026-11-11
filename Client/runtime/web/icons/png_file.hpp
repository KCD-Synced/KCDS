// An RGBA picture as a .png file a web page shows: 8-bit RGBA, no filtering, the image data in
// stored (uncompressed) deflate blocks. An icon is small, so its file stays small enough for a
// page to load at once, and no compression library is needed. Pure code.
#pragma once

#include <cstdint>
#include <vector>

#include "dds_picture.hpp"

namespace kcds::runtime::web::icons {

// Empty for an empty or inconsistent picture.
std::vector<std::uint8_t> png_file(const Picture& picture);

}  // namespace kcds::runtime::web::icons
