#include "kcd2s/voice/frame.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

extern "C" {
#include "third_party/monocypher/monocypher.h"
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kcd2s::voice {
namespace {

void write_u32(std::uint8_t* to, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    to[i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

void write_u64(std::uint8_t* to, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    to[i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

std::uint32_t read_u32(const std::uint8_t* from) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(from[i]) << (8 * i);
  }
  return value;
}

std::uint64_t read_u64(const std::uint8_t* from) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(from[i]) << (8 * i);
  }
  return value;
}

// XChaCha20's 24 byte nonce: the direction and id, then the counter. The same key and counter in
// the other direction, or from another talker, is another nonce.
void nonce_of(const VoiceHeader& header, std::uint8_t nonce[24]) {
  std::memset(nonce, 0, 24);
  nonce[0] = header.tag;
  write_u32(nonce + 1, header.id);
  write_u64(nonce + 8, header.counter);
}

}  // namespace

void secure_random(std::uint8_t* out, std::size_t size) {
#ifdef _WIN32
  const auto status =
      BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status != 0) {
    throw std::runtime_error("No secure random source for voice keys");
  }
#else
  const int source = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (source < 0) {
    throw std::runtime_error("No secure random source for voice keys");
  }
  std::size_t done = 0;
  while (done < size) {
    const ssize_t got = read(source, out + done, size - done);
    if (got <= 0) {
      close(source);
      throw std::runtime_error("No secure random source for voice keys");
    }
    done += static_cast<std::size_t>(got);
  }
  close(source);
#endif
}

KeyPair make_key_pair() {
  KeyPair pair;
  secure_random(pair.secret_key.data(), pair.secret_key.size());
  crypto_x25519_public_key(pair.public_key.data(), pair.secret_key.data());
  return pair;
}

bool derive_session_key(const Key& own_secret, const Key& peer_public, const Key& client_public,
                        const Key& server_public, Key& session_key) {
  std::uint8_t shared[32];
  crypto_x25519(shared, own_secret.data(), peer_public.data());
  std::uint8_t any = 0;
  for (const std::uint8_t byte : shared) {
    any |= byte;
  }
  if (any == 0) {
    crypto_wipe(shared, sizeof(shared));
    return false;
  }
  std::uint8_t mixed[64];
  std::memcpy(mixed, client_public.data(), 32);
  std::memcpy(mixed + 32, server_public.data(), 32);
  crypto_blake2b_keyed(session_key.data(), session_key.size(), shared, sizeof(shared), mixed,
                       sizeof(mixed));
  crypto_wipe(shared, sizeof(shared));
  return true;
}

bool read_voice_header(const std::uint8_t* data, std::size_t size, VoiceHeader& out) {
  if (!is_voice_datagram(data, size) || size < kVoiceHeaderBytes + kMacBytes ||
      size > kMaxVoiceDatagram) {
    return false;
  }
  out.tag = data[0];
  out.id = read_u32(data + 1);
  out.counter = read_u64(data + 5);
  out.loudness = data[13];
  return true;
}

bool seal_voice(const VoiceHeader& header, const Key& key, const std::uint8_t* payload,
                std::size_t payload_size, std::vector<std::uint8_t>& out) {
  if (payload_size > kMaxVoicePayload ||
      (header.tag != kVoiceUpTag && header.tag != kVoiceDownTag)) {
    return false;
  }
  out.resize(kVoiceHeaderBytes + kMacBytes + payload_size);
  out[0] = header.tag;
  write_u32(out.data() + 1, header.id);
  write_u64(out.data() + 5, header.counter);
  out[13] = header.loudness;
  std::uint8_t nonce[24];
  nonce_of(header, nonce);
  crypto_aead_lock(out.data() + kVoiceHeaderBytes + kMacBytes, out.data() + kVoiceHeaderBytes,
                   key.data(), nonce, out.data(), kVoiceHeaderBytes, payload, payload_size);
  return true;
}

bool open_voice(const std::uint8_t* data, std::size_t size, const VoiceHeader& header,
                const Key& key, std::vector<std::uint8_t>& payload) {
  payload.clear();
  if (size < kVoiceHeaderBytes + kMacBytes || size > kMaxVoiceDatagram) {
    return false;
  }
  const std::size_t text = size - kVoiceHeaderBytes - kMacBytes;
  payload.resize(text);
  std::uint8_t nonce[24];
  nonce_of(header, nonce);
  const int refused =
      crypto_aead_unlock(payload.data(), data + kVoiceHeaderBytes, key.data(), nonce, data,
                         kVoiceHeaderBytes, data + kVoiceHeaderBytes + kMacBytes, text);
  if (refused != 0) {
    payload.clear();
    return false;
  }
  return true;
}

bool ReplayWindow::may_accept(std::uint64_t counter) const {
  if (!any_ || counter > newest_) {
    return true;
  }
  const std::uint64_t behind = newest_ - counter;
  return behind < 64 && (seen_ & (std::uint64_t{1} << behind)) == 0;
}

void ReplayWindow::mark(std::uint64_t counter) {
  if (!any_) {
    any_ = true;
    newest_ = counter;
    seen_ = 1;
    return;
  }
  if (counter > newest_) {
    const std::uint64_t moved = counter - newest_;
    seen_ = moved >= 64 ? 0 : seen_ << moved;
    seen_ |= 1;
    newest_ = counter;
    return;
  }
  const std::uint64_t behind = newest_ - counter;
  if (behind < 64) {
    seen_ |= std::uint64_t{1} << behind;
  }
}

}  // namespace kcd2s::voice
