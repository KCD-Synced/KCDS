#pragma once
// The server's name for the client's watermark: the name the server browser lists
// (KCDS_SERVER_NAME), told to a player once their world is ready.
//
//   SERVER_NAME : server -> client: [tag][u8 length][the name, UTF-8, 1 to 64 bytes].
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kcd2s::integration {

inline constexpr std::uint8_t kServerNameTag = 171;
inline constexpr std::size_t kMaxServerNameLength = 64;

inline std::vector<std::uint8_t> encode_server_name(const std::string& name) {
  const std::size_t length =
      name.size() < kMaxServerNameLength ? name.size() : kMaxServerNameLength;
  std::vector<std::uint8_t> out;
  out.reserve(2 + length);
  out.push_back(kServerNameTag);
  out.push_back(static_cast<std::uint8_t>(length));
  out.insert(out.end(), name.begin(), name.begin() + static_cast<std::ptrdiff_t>(length));
  return out;
}

inline bool decode_server_name(const std::vector<std::uint8_t>& in, std::string& name) {
  if (in.size() < 2 || in[0] != kServerNameTag) {
    return false;
  }
  const std::size_t length = in[1];
  if (length == 0 || length > kMaxServerNameLength || in.size() != 2 + length) {
    return false;
  }
  name.assign(in.begin() + 2, in.end());
  return true;
}

}  // namespace kcd2s::integration
