#ifndef KCD2S_CORE_VERSION_HPP
#define KCD2S_CORE_VERSION_HPP

#include <array>
#include <cstdint>

// The build's handshake key, injected by CMake: SHA-256 of the release build secret
// and the wire label (KCD2S_BUILD_SALT), derived at configure time. Only these 32 bytes are
// compiled in, never the secret or the label, and SHA-256 does not run backwards, so the salt
// cannot be worked out from a binary. A client and a server interoperate only when built with the
// same key: the handshake's CHALLENGE_RESPONSE carries an HMAC under it (transport/handshake.hpp).
//
// KCDS_BUILD_FINGERPRINT (32 bits of a hash of the key) travels in CONNECT_REQUEST so a client of
// another build is turned away early; it reveals nothing about the key. The fallbacks keep a lone
// translation unit (a test not linking kcd2s::flags) compiling; CMake always sets both.
#ifndef KCDS_HANDSHAKE_KEY
#define KCDS_HANDSHAKE_KEY                                                                        \
  0x6b, 0x63, 0x64, 0x73, 0x2d, 0x64, 0x65, 0x76, 0x2d, 0x6b, 0x65, 0x79, 0x00, 0x00, 0x00, 0x00, \
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   \
      0x00
#endif
#ifndef KCDS_BUILD_FINGERPRINT
#define KCDS_BUILD_FINGERPRINT 0x6b636473u
#endif

namespace kcd2s::core {

// Returns the KCD2-Synced core library version string.
const char* version();

// The build's 32-byte handshake key.
inline constexpr std::array<std::uint8_t, 32> kHandshakeKey = {KCDS_HANDSHAKE_KEY};
inline const std::array<std::uint8_t, 32>& handshake_key() {
  return kHandshakeKey;
}

// The build fingerprint carried in the KSTP CONNECT_REQUEST: the server turns away a client whose
// fingerprint is not its own build's. A quick compatibility gate; the HMAC proof is the real check.
constexpr std::uint32_t kBuildFingerprint = KCDS_BUILD_FINGERPRINT;
constexpr std::uint32_t build_fingerprint() {
  return kBuildFingerprint;
}

}  // namespace kcd2s::core

#endif  // KCD2S_CORE_VERSION_HPP
