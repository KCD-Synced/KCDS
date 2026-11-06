// Voice datagrams and their encryption. Input is untrusted: a bad datagram
// is refused before any expensive work, and nothing here logs a key.
#ifndef KCD2S_VOICE_FRAME_HPP
#define KCD2S_VOICE_FRAME_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kcd2s::voice {

using Key = std::array<std::uint8_t, 32>;

struct KeyPair {
  Key public_key{};
  Key secret_key{};
};

// Bytes from the system's secure random source. Throws when it has none.
void secure_random(std::uint8_t* out, std::size_t size);

// A new X25519 key pair from the system's secure random source. Throws when it has none.
KeyPair make_key_pair();

// The session key both ends derive from the X25519 agreement and both public keys. False when
// the peer's public key gives an all-zero secret (a low order point: a refused peer).
bool derive_session_key(const Key& own_secret, const Key& peer_public, const Key& client_public,
                        const Key& server_public, Key& session_key);

// First byte of a datagram on the game's UDP port. KSTP uses other values.
constexpr std::uint8_t kVoiceUpTag = 0xFA;
constexpr std::uint8_t kVoiceDownTag = 0xFB;

// The largest Opus frame carried, far above 24 kbps at 20 ms; a longer one is refused.
constexpr std::size_t kMaxVoicePayload = 512;
constexpr std::size_t kMacBytes = 16;
// tag, session or talker id, counter, loudness.
constexpr std::size_t kVoiceHeaderBytes = 1 + 4 + 8 + 1;
constexpr std::size_t kMaxVoiceDatagram = kVoiceHeaderBytes + kMacBytes + kMaxVoicePayload;

// What the sender's end of a datagram says, before its encryption is checked.
struct VoiceHeader {
  std::uint8_t tag = 0;
  // Up: the sender's session. Down: the talker.
  std::uint32_t id = 0;
  std::uint64_t counter = 0;
  std::uint8_t loudness = 0;
};

inline bool is_voice_datagram(const std::uint8_t* data, std::size_t size) {
  return size > 0 && (data[0] == kVoiceUpTag || data[0] == kVoiceDownTag);
}

// The header of a datagram of a possible length. Cheap: no cryptography.
bool read_voice_header(const std::uint8_t* data, std::size_t size, VoiceHeader& out);

// Encrypts a frame. `out` is replaced, its memory reused. False for a payload too long.
bool seal_voice(const VoiceHeader& header, const Key& key, const std::uint8_t* payload,
                std::size_t payload_size, std::vector<std::uint8_t>& out);

// Checks and decrypts a datagram whose header was read. False when it was changed or the key is
// wrong; `payload` is then empty.
bool open_voice(const std::uint8_t* data, std::size_t size, const VoiceHeader& header,
                const Key& key, std::vector<std::uint8_t>& payload);

// Which counters were seen: the newest and the 64 before it. A repeat, or one older than that, is
// refused.
class ReplayWindow {
 public:
  // Whether this counter could be new. Cheap, for before the cryptography.
  bool may_accept(std::uint64_t counter) const;
  // Marks it seen, after the datagram's encryption checked out.
  void mark(std::uint64_t counter);

 private:
  std::uint64_t newest_ = 0;
  std::uint64_t seen_ = 0;
  bool any_ = false;
};

}  // namespace kcd2s::voice

#endif
