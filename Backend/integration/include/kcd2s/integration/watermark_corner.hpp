#pragma once
#include <cstdint>
#include <vector>

// The corner of the screen a player's game draws the KCDS watermark in (Watermark.setCorner). The
// watermark itself cannot be turned off; a server only moves it.
//
//   WATERMARK_CORNER : server -> a player's client, on the control channel (0): [tag][u8 corner]
//                      (0 bottom left, 1 bottom right, 2 top right, 3 top left). Sent when it
//                      changes and, while it is not the bottom left one, to a player whose world
//                      got ready; a client goes back to the bottom left corner when it connects.
namespace kcd2s::integration {

inline constexpr std::uint8_t kWatermarkCornerTag = 178;

enum class WatermarkCorner : std::uint8_t {
  BottomLeft = 0,
  BottomRight = 1,
  TopRight = 2,
  TopLeft = 3,
};

inline std::vector<std::uint8_t> encode_watermark_corner(WatermarkCorner corner) {
  return {kWatermarkCornerTag, std::uint8_t(corner)};
}

inline bool decode_watermark_corner(const std::vector<std::uint8_t>& p, WatermarkCorner& corner) {
  if (p.size() != 2 || p[0] != kWatermarkCornerTag ||
      p[1] > std::uint8_t(WatermarkCorner::TopLeft)) {
    return false;
  }
  corner = WatermarkCorner(p[1]);
  return true;
}

}  // namespace kcd2s::integration
