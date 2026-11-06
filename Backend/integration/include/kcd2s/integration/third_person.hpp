#pragma once
#include <cstdint>
#include <vector>

// The third-person camera of the local player (KCDS addition ThirdPerson.setMode).
//
//   THIRD_PERSON : server -> a player's client, on the control channel (0): [tag][u8 mode].
//                  mode 0 = allowed (the F5 key toggles between the views), 1 = disabled (first
//                  person only), 2 = forced (third person only). Sent when the server's mode
//                  changes and, while it is not "allowed", to a player whose world got ready; a
//                  client goes back to "allowed" whenever it leaves a world.
namespace kcd2s::integration {

inline constexpr std::uint8_t kThirdPersonTag = 58;

inline constexpr std::uint8_t kThirdPersonAllowed = 0;
inline constexpr std::uint8_t kThirdPersonDisabled = 1;
inline constexpr std::uint8_t kThirdPersonForced = 2;

inline std::vector<std::uint8_t> encode_third_person(std::uint8_t mode) {
  return {kThirdPersonTag, mode};
}

inline bool decode_third_person(const std::vector<std::uint8_t>& p, std::uint8_t& mode) {
  if (p.size() != 2 || p[0] != kThirdPersonTag || p[1] > kThirdPersonForced) {
    return false;
  }
  mode = p[1];
  return true;
}

}  // namespace kcd2s::integration
