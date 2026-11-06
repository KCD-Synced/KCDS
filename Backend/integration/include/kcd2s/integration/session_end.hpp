#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The server ends a player's session and says why, so the client goes back to the
// main menu at once with that message instead of noticing the silence ten seconds later.
//
//   SESSION_END : server -> a player's client, on the control channel (0):
//                 [tag][u8 kind][u16 length, little-endian][reason, UTF-8, up to 200 bytes].
//                 kind 1: the server is shutting down; kind 2: a resource kicked the player;
//                 kind 3: a resource let the player go (Player.disconnect): the
//                 reason is shown as it is, without "kicked"; kind 4: the same, and the client
//                 closes the game; kind 5: the server's own bans name the player (Player.ban,
//                 Bans): the client says it is banned and offers no reconnect.
//                 The server drops the player shortly after, so the message has time to arrive.
namespace kcd2s::integration {

inline constexpr std::uint8_t kSessionEndTag = 52;
inline constexpr std::size_t kSessionEndReasonMax = 200;

enum class SessionEndKind : std::uint8_t {
  Closed = 1,
  Kicked = 2,
  Left = 3,
  Quit = 4,
  Banned = 5,
};

struct SessionEnd {
  SessionEndKind kind = SessionEndKind::Closed;
  std::string reason;
};

// Empty when the reason is longer than kSessionEndReasonMax bytes.
inline std::vector<std::uint8_t> encode_session_end(const SessionEnd& end) {
  if (end.reason.size() > kSessionEndReasonMax) {
    return {};
  }
  std::vector<std::uint8_t> p{kSessionEndTag, std::uint8_t(end.kind),
                              std::uint8_t(end.reason.size() & 0xFF),
                              std::uint8_t(end.reason.size() >> 8)};
  p.insert(p.end(), end.reason.begin(), end.reason.end());
  return p;
}

inline bool decode_session_end(const std::vector<std::uint8_t>& p, SessionEnd& end) {
  if (p.size() < 4 || p[0] != kSessionEndTag) {
    return false;
  }
  if (p[1] < std::uint8_t(SessionEndKind::Closed) || p[1] > std::uint8_t(SessionEndKind::Banned)) {
    return false;
  }
  const std::size_t length = std::size_t(p[2]) | (std::size_t(p[3]) << 8);
  if (length > kSessionEndReasonMax || p.size() != 4 + length) {
    return false;
  }
  end.kind = SessionEndKind(p[1]);
  end.reason.assign(p.begin() + 4, p.end());
  return true;
}

}  // namespace kcd2s::integration
