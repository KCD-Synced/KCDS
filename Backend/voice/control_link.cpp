#include "kcd2s/voice/control_link.hpp"

#include <cstring>

extern "C" {
#include "third_party/monocypher/monocypher.h"
}

namespace kcd2s::voice {
namespace {

constexpr std::size_t kFrameHeader = 4 + 8 + 16;

class Writer {
 public:
  void u8(std::uint8_t value) {
    bytes.push_back(value);
  }
  void u16(std::uint16_t value) {
    raw(&value, 2);
  }
  void u32(std::uint32_t value) {
    raw(&value, 4);
  }
  void f32(float value) {
    raw(&value, 4);
  }
  void raw(const void* data, std::size_t size) {
    const auto* from = static_cast<const std::uint8_t*>(data);
    bytes.insert(bytes.end(), from, from + size);
  }
  std::vector<std::uint8_t> bytes;
};

class Reader {
 public:
  explicit Reader(const std::vector<std::uint8_t>& from) : data(from) {
  }
  bool u8(std::uint8_t& value) {
    return raw(&value, 1);
  }
  bool u16(std::uint16_t& value) {
    return raw(&value, 2);
  }
  bool u32(std::uint32_t& value) {
    return raw(&value, 4);
  }
  bool f32(float& value) {
    return raw(&value, 4);
  }
  bool raw(void* to, std::size_t size) {
    if (at + size > data.size()) {
      return false;
    }
    std::memcpy(to, data.data() + at, size);
    at += size;
    return true;
  }
  bool done() const {
    return at == data.size();
  }
  const std::vector<std::uint8_t>& data;
  std::size_t at = 0;
};

void nonce_of(std::uint64_t counter, std::uint8_t nonce[24]) {
  std::memset(nonce, 0, 24);
  std::memcpy(nonce, &counter, 8);
}

}  // namespace

std::vector<std::uint8_t> encode_control(const ControlMessage& message) {
  Writer out;
  out.u8(static_cast<std::uint8_t>(message.kind));
  switch (message.kind) {
    case ControlKind::Hello:
      out.u16(std::uint16_t(message.server_id.size()));
      out.raw(message.server_id.data(), message.server_id.size());
      break;
    case ControlKind::Welcome:
      out.raw(message.key.data(), message.key.size());
      out.u16(message.port);
      out.u32(message.session);
      break;
    case ControlKind::Join:
      out.u32(message.player);
      out.u32(message.session);
      out.raw(message.key.data(), message.key.size());
      break;
    case ControlKind::Leave:
      out.u32(message.player);
      break;
    case ControlKind::Positions:
      out.u32(std::uint32_t(message.positions.size()));
      for (const ControlPosition& entry : message.positions) {
        out.u32(entry.player);
        out.f32(entry.position.x);
        out.f32(entry.position.y);
        out.f32(entry.position.z);
        out.u32(entry.world);
      }
      break;
    case ControlKind::Enabled:
      out.u8(message.flag ? 1 : 0);
      break;
    case ControlKind::Range:
      out.u8(message.tier);
      out.f32(message.meters);
      break;
    case ControlKind::Tier:
      out.u32(message.player);
      out.u8(message.tier);
      break;
    case ControlKind::Muted:
    case ControlKind::Deaf:
    case ControlKind::Talking:
      out.u32(message.player);
      out.u8(message.flag ? 1 : 0);
      break;
    case ControlKind::ListenerMute:
      out.u32(message.player);
      out.u32(message.other);
      out.u8(message.flag ? 1 : 0);
      break;
    case ControlKind::Synced:
    case ControlKind::Heartbeat:
      break;
  }
  return out.bytes;
}

std::optional<ControlMessage> decode_control(const std::vector<std::uint8_t>& bytes) {
  Reader in(bytes);
  ControlMessage message;
  std::uint8_t kind = 0;
  std::uint8_t flag = 0;
  if (!in.u8(kind)) {
    return std::nullopt;
  }
  message.kind = static_cast<ControlKind>(kind);
  bool good = false;
  switch (message.kind) {
    case ControlKind::Hello: {
      std::uint16_t size = 0;
      good = in.u16(size) && size <= 256;
      if (good) {
        message.server_id.resize(size);
        good = in.raw(message.server_id.data(), size);
      }
      break;
    }
    case ControlKind::Welcome:
      good = in.raw(message.key.data(), message.key.size()) && in.u16(message.port) &&
             in.u32(message.session);
      break;
    case ControlKind::Join:
      good = in.u32(message.player) && in.u32(message.session) &&
             in.raw(message.key.data(), message.key.size());
      break;
    case ControlKind::Leave:
      good = in.u32(message.player);
      break;
    case ControlKind::Positions: {
      std::uint32_t count = 0;
      good = in.u32(count) && count <= kMaxControlBytes / 20;
      for (std::uint32_t i = 0; good && i < count; ++i) {
        ControlPosition entry;
        good = in.u32(entry.player) && in.f32(entry.position.x) && in.f32(entry.position.y) &&
               in.f32(entry.position.z) && in.u32(entry.world);
        message.positions.push_back(entry);
      }
      break;
    }
    case ControlKind::Enabled:
      good = in.u8(flag);
      break;
    case ControlKind::Range:
      good = in.u8(message.tier) && in.f32(message.meters) && message.tier < 3;
      break;
    case ControlKind::Tier:
      good = in.u32(message.player) && in.u8(message.tier) && message.tier < 3;
      break;
    case ControlKind::Muted:
    case ControlKind::Deaf:
    case ControlKind::Talking:
      good = in.u32(message.player) && in.u8(flag);
      break;
    case ControlKind::ListenerMute:
      good = in.u32(message.player) && in.u32(message.other) && in.u8(flag);
      break;
    case ControlKind::Synced:
    case ControlKind::Heartbeat:
      good = true;
      break;
    default:
      return std::nullopt;
  }
  if (!good || !in.done()) {
    return std::nullopt;
  }
  message.flag = flag != 0;
  return message;
}

ControlRandom make_control_random() {
  ControlRandom random{};
  secure_random(random.data(), random.size());
  return random;
}

Key control_key(const std::string& secret, bool from_game, const std::string& server_id,
                const ControlRandom& game_random, const ControlRandom& voice_random) {
  Writer what;
  const char* direction = from_game ? "kcds-voice game to voice" : "kcds-voice voice to game";
  what.raw(direction, std::strlen(direction));
  what.u16(std::uint16_t(server_id.size()));
  what.raw(server_id.data(), server_id.size());
  what.raw(game_random.data(), game_random.size());
  what.raw(voice_random.data(), voice_random.size());
  // A secret of any length: its hash is the hash key.
  std::uint8_t secret_hash[32];
  crypto_blake2b(secret_hash, sizeof(secret_hash),
                 reinterpret_cast<const std::uint8_t*>(secret.data()), secret.size());
  Key key{};
  crypto_blake2b_keyed(key.data(), key.size(), secret_hash, sizeof(secret_hash), what.bytes.data(),
                       what.bytes.size());
  crypto_wipe(secret_hash, sizeof(secret_hash));
  return key;
}

std::vector<std::uint8_t> encode_control_opening(const std::string& server_id,
                                                 const ControlRandom& game_random) {
  Writer out;
  out.u16(std::uint16_t(server_id.size()));
  out.raw(server_id.data(), server_id.size());
  out.raw(game_random.data(), game_random.size());
  return out.bytes;
}

int decode_control_opening(const std::vector<std::uint8_t>& bytes, std::string& server_id,
                           ControlRandom& game_random, std::size_t& used) {
  if (bytes.size() < 2) {
    return 0;
  }
  std::uint16_t size = 0;
  std::memcpy(&size, bytes.data(), 2);
  if (size == 0 || size > 256) {
    return -1;
  }
  used = 2u + size + game_random.size();
  if (bytes.size() < used) {
    return 0;
  }
  server_id.assign(reinterpret_cast<const char*>(bytes.data()) + 2, size);
  std::memcpy(game_random.data(), bytes.data() + 2 + size, game_random.size());
  return 1;
}

std::vector<std::uint8_t> ControlSealer::seal(const std::vector<std::uint8_t>& plain) {
  std::vector<std::uint8_t> frame(kFrameHeader + plain.size());
  const std::uint32_t size = std::uint32_t(plain.size());
  const std::uint64_t counter = ++counter_;
  std::memcpy(frame.data(), &size, 4);
  std::memcpy(frame.data() + 4, &counter, 8);
  std::uint8_t nonce[24];
  nonce_of(counter, nonce);
  crypto_aead_lock(frame.data() + kFrameHeader, frame.data() + 12, key_.data(), nonce, frame.data(),
                   12, plain.data(), plain.size());
  return frame;
}

bool ControlOpener::feed(const std::uint8_t* data, std::size_t size) {
  buffer_.insert(buffer_.end(), data, data + size);
  while (buffer_.size() >= 4) {
    std::uint32_t length = 0;
    std::memcpy(&length, buffer_.data(), 4);
    if (length > kMaxControlBytes) {
      return false;
    }
    if (buffer_.size() < kFrameHeader + length) {
      return true;
    }
    std::uint64_t counter = 0;
    std::memcpy(&counter, buffer_.data() + 4, 8);
    if (counter <= last_counter_) {
      return false;
    }
    std::uint8_t nonce[24];
    nonce_of(counter, nonce);
    std::vector<std::uint8_t> plain(length);
    if (crypto_aead_unlock(plain.data(), buffer_.data() + 12, key_.data(), nonce, buffer_.data(),
                           12, buffer_.data() + kFrameHeader, length) != 0) {
      return false;
    }
    last_counter_ = counter;
    ready_.push_back(std::move(plain));
    buffer_.erase(buffer_.begin(), buffer_.begin() + std::ptrdiff_t(kFrameHeader + length));
  }
  return true;
}

std::optional<std::vector<std::uint8_t>> ControlOpener::next() {
  if (ready_.empty()) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> first = std::move(ready_.front());
  ready_.erase(ready_.begin());
  return first;
}

}  // namespace kcd2s::voice
