#pragma once
#include <cstdint>
#include <vector>

// Whether a player may fly with NoClip (KCDS addition Player.noclipAllowed).
//
//   NOCLIP_ALLOWED : server -> a player's client, on the control channel (0):
//                    [tag][u8 allowed 0/1]. Sent when a resource allows or forbids it and, while
//                    it is forbidden, to a player whose world got ready. While forbidden the client
//                    refuses NoClip.enable and ends a flight that runs. A client allows it again
//                    whenever it leaves a world.
namespace kcd2s::integration {

inline constexpr std::uint8_t kNoClipAllowedTag = 164;

inline std::vector<std::uint8_t> encode_noclip_allowed(bool allowed) {
  return {kNoClipAllowedTag, std::uint8_t(allowed ? 1 : 0)};
}

inline bool decode_noclip_allowed(const std::vector<std::uint8_t>& p, bool& allowed) {
  if (p.size() != 2 || p[0] != kNoClipAllowedTag || p[1] > 1) {
    return false;
  }
  allowed = p[1] == 1;
  return true;
}

}  // namespace kcd2s::integration
