#ifndef KCD2S_CORE_CRYPTO_SHA256_HPP
#define KCD2S_CORE_CRYPTO_SHA256_HPP

// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), our own implementation: the handshake's build
// proof (handshake_key.hpp) is an HMAC under the build's key. No external crypto
// library is linked.

#include <array>
#include <cstddef>
#include <cstdint>

namespace kcd2s::core::crypto {

using Sha256Digest = std::array<std::uint8_t, 32>;

// Streaming SHA-256: feed bytes with update(), then finish() once.
class Sha256 {
 public:
  Sha256();
  void update(const std::uint8_t* data, std::size_t length);
  Sha256Digest finish();

 private:
  void process(const std::uint8_t* block);

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

// SHA-256 of one buffer.
Sha256Digest sha256(const std::uint8_t* data, std::size_t length);

// HMAC-SHA256 of `message` under `key` (any length; a key longer than 64 bytes is hashed first).
Sha256Digest hmac_sha256(const std::uint8_t* key, std::size_t key_length,
                         const std::uint8_t* message, std::size_t message_length);

// Compares two buffers in time that does not depend on where they differ.
bool equal_in_constant_time(const std::uint8_t* a, const std::uint8_t* b, std::size_t length);

}  // namespace kcd2s::core::crypto

#endif  // KCD2S_CORE_CRYPTO_SHA256_HPP
