#include "kcd2s/core/crypto/sha256.hpp"

#include <algorithm>
#include <cstring>

namespace kcd2s::core::crypto {
namespace {

// FIPS 180-4 section 4.2.2: the first 32 bits of the fractional parts of the cube roots of the
// first 64 primes.
constexpr std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::size_t kBlockBytes = 64;

std::uint32_t rotate_right(std::uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

}  // namespace

Sha256::Sha256()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {
}

void Sha256::process(const std::uint8_t* block) {
  std::uint32_t schedule[64];
  for (int i = 0; i < 16; ++i) {
    schedule[i] = (std::uint32_t(block[4 * i]) << 24) | (std::uint32_t(block[4 * i + 1]) << 16) |
                  (std::uint32_t(block[4 * i + 2]) << 8) | std::uint32_t(block[4 * i + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotate_right(schedule[i - 15], 7) ^
                             rotate_right(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
    const std::uint32_t s1 = rotate_right(schedule[i - 2], 17) ^ rotate_right(schedule[i - 2], 19) ^
                             (schedule[i - 2] >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t first = h + sum1 + choose + kRound[i] + schedule[i];
    const std::uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t second = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + first;
    d = c;
    c = b;
    b = a;
    a = first + second;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t length) {
  total_bytes_ += length;
  while (length > 0) {
    const std::size_t take = std::min(length, kBlockBytes - buffered_);
    std::memcpy(buffer_.data() + buffered_, data, take);
    buffered_ += take;
    data += take;
    length -= take;
    if (buffered_ == kBlockBytes) {
      process(buffer_.data());
      buffered_ = 0;
    }
  }
}

Sha256Digest Sha256::finish() {
  const std::uint64_t total_bits = total_bytes_ * 8;
  const std::uint8_t one = 0x80;
  const std::uint8_t zero = 0;
  update(&one, 1);
  while (buffered_ != kBlockBytes - 8) {
    update(&zero, 1);
  }
  std::uint8_t length[8];
  for (int i = 0; i < 8; ++i) {
    length[i] = std::uint8_t(total_bits >> (56 - 8 * i));
  }
  update(length, 8);
  Sha256Digest digest{};
  for (int i = 0; i < 8; ++i) {
    digest[4 * i] = std::uint8_t(state_[i] >> 24);
    digest[4 * i + 1] = std::uint8_t(state_[i] >> 16);
    digest[4 * i + 2] = std::uint8_t(state_[i] >> 8);
    digest[4 * i + 3] = std::uint8_t(state_[i]);
  }
  return digest;
}

Sha256Digest sha256(const std::uint8_t* data, std::size_t length) {
  Sha256 hash;
  hash.update(data, length);
  return hash.finish();
}

Sha256Digest hmac_sha256(const std::uint8_t* key, std::size_t key_length,
                         const std::uint8_t* message, std::size_t message_length) {
  std::uint8_t block[kBlockBytes] = {};
  if (key_length > kBlockBytes) {
    const Sha256Digest hashed = sha256(key, key_length);
    std::memcpy(block, hashed.data(), hashed.size());
  } else if (key_length > 0) {
    std::memcpy(block, key, key_length);
  }
  std::uint8_t inner_pad[kBlockBytes];
  std::uint8_t outer_pad[kBlockBytes];
  for (std::size_t i = 0; i < kBlockBytes; ++i) {
    inner_pad[i] = std::uint8_t(block[i] ^ 0x36);
    outer_pad[i] = std::uint8_t(block[i] ^ 0x5c);
  }
  Sha256 inner;
  inner.update(inner_pad, kBlockBytes);
  inner.update(message, message_length);
  const Sha256Digest inner_digest = inner.finish();
  Sha256 outer;
  outer.update(outer_pad, kBlockBytes);
  outer.update(inner_digest.data(), inner_digest.size());
  return outer.finish();
}

bool equal_in_constant_time(const std::uint8_t* a, const std::uint8_t* b, std::size_t length) {
  std::uint8_t difference = 0;
  for (std::size_t i = 0; i < length; ++i) {
    difference = std::uint8_t(difference | (a[i] ^ b[i]));
  }
  return difference == 0;
}

}  // namespace kcd2s::core::crypto
