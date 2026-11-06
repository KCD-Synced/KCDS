// The control link between a game server and a standalone kcds-voice.
// One TCP connection; every message is encrypted and authenticated with a
// key made from the shared secret, so someone without the secret can neither read nor send one.
// Messages are small and only sent on change, positions as a batch at 10 Hz.
#ifndef KCD2S_VOICE_CONTROL_LINK_HPP
#define KCD2S_VOICE_CONTROL_LINK_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/core/math.hpp"
#include "kcd2s/voice/frame.hpp"

namespace kcd2s::voice {

enum class ControlKind : std::uint8_t {
  Hello = 1,          // game -> voice: server_id
  Welcome = 2,        // voice -> game: its public key, its UDP port, this tenant's session base
  Join = 3,           // game -> voice: player, session, the client's public key
  Leave = 4,          // game -> voice: player
  Positions = 5,      // game -> voice: (player, x, y, z, world)...
  Enabled = 6,        // game -> voice: flag
  Range = 7,          // game -> voice: tier, meters
  Tier = 8,           // game -> voice: player, tier
  Muted = 9,          // game -> voice: player, flag
  Deaf = 10,          // game -> voice: player, flag
  ListenerMute = 11,  // game -> voice: listener, talker, flag
  Talking = 12,       // voice -> game: player, flag
  Synced = 13,        // game -> voice: all Joins are sent; players it did not name are gone
  Heartbeat = 14,     // both ways, when there was nothing else to send for a second
};

struct ControlPosition {
  std::uint32_t player = 0;
  core::Vec3 position{};
  std::uint32_t world = 0;
};

struct ControlMessage {
  ControlKind kind = ControlKind::Hello;
  std::string server_id;
  Key key{};
  std::uint16_t port = 0;
  std::uint32_t session = 0;
  std::uint32_t player = 0;
  std::uint32_t other = 0;
  bool flag = false;
  std::uint8_t tier = 0;
  float meters = 0.0f;
  std::vector<ControlPosition> positions;
};

std::vector<std::uint8_t> encode_control(const ControlMessage& message);
std::optional<ControlMessage> decode_control(const std::vector<std::uint8_t>& bytes);

// The longest message on the link: a whole server's positions and some.
constexpr std::size_t kMaxControlBytes = 64 * 1024;

// What each end says first, before anything is encrypted. A new one for each connection.
using ControlRandom = std::array<std::uint8_t, 32>;
ControlRandom make_control_random();

// The key of one direction of one connection, from the tenant's secret and both ends' randoms: a
// captured connection cannot be played to a new one, and one side's messages not back to it.
Key control_key(const std::string& secret, bool from_game, const std::string& server_id,
                const ControlRandom& game_random, const ControlRandom& voice_random);

// How a connection starts: the game server sends this, then the voice server its random; from there
// on only frames.
std::vector<std::uint8_t> encode_control_opening(const std::string& server_id,
                                                 const ControlRandom& game_random);
// Whether `bytes` starts with a whole opening: 1 yes, 0 not yet, -1 never.
int decode_control_opening(const std::vector<std::uint8_t>& bytes, std::string& server_id,
                           ControlRandom& game_random, std::size_t& used);

// Frames one way of a link: [u32 size][u64 counter][16 tag][ciphertext]. Each frame's counter must
// rise, so a captured one cannot be sent again.
class ControlSealer {
 public:
  explicit ControlSealer(const Key& key) : key_(key) {
  }
  std::vector<std::uint8_t> seal(const std::vector<std::uint8_t>& plain);

 private:
  Key key_;
  std::uint64_t counter_ = 0;
};

class ControlOpener {
 public:
  explicit ControlOpener(const Key& key) : key_(key) {
  }
  // Feeds received bytes. False when the stream is bad: wrong secret, a change or a repeat. The
  // link is then to be closed.
  bool feed(const std::uint8_t* data, std::size_t size);
  // The next whole message, if there is one.
  std::optional<std::vector<std::uint8_t>> next();

 private:
  Key key_;
  std::uint64_t last_counter_ = 0;
  std::vector<std::uint8_t> buffer_;
  std::vector<std::vector<std::uint8_t>> ready_;
};

}  // namespace kcd2s::voice

#endif
