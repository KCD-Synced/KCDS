#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace kcd2s::local {
// WHGame 1.5.5 StartAnimationByID copies exactly these 64 value bytes.
// no engine pointers cross the network.
struct AnimationParams {
  float transition{}, normalized_time{}, speed{1}, weight{1};
  std::uint32_t layer{}, token{}, flags{}, user_token{};
  float user_data[8]{};
};
static_assert(sizeof(AnimationParams) == 64);
struct AnimationLayer {
  std::uint64_t sequence{};
  std::int32_t clip{-1};
  AnimationParams params{};
};
using AnimationState = std::array<AnimationLayer, 16>;
// Native StopAnimation may leave its fading clip in the FIFO. Discovering that
// clip again is not a new start. An explicit start (including the same ID) lifts
// the fence, so two intentional punches remain two punches.
struct AnimationStopFence {
  std::array<std::int32_t, 16> stopped{};
  AnimationStopFence() {
    stopped.fill(-2);
  }
  void stop(unsigned layer, std::int32_t clip) {
    if (layer < 16 && clip >= 0)
      stopped[layer] = clip;
  }
  void start(unsigned layer) {
    if (layer < 16)
      stopped[layer] = -2;
  }
  bool fading(unsigned layer, std::int32_t clip) const {
    return layer < 16 && stopped[layer] == clip;
  }
};
inline bool valid_animation(const AnimationLayer& a, unsigned layer) {
  if (!a.sequence)
    return true;
  // A player woman's set goes on after the male list with the female clips: ids past
  // 300000.
  if (a.clip < -1 || a.clip > 0xFFFFF || a.params.layer != layer)
    return false;
  const auto& p = a.params;
  if (!std::isfinite(p.transition) || p.transition < 0 || p.transition > 30 ||
      !std::isfinite(p.normalized_time) || p.normalized_time < 0 || p.normalized_time > 1 ||
      !std::isfinite(p.speed) || p.speed < 0 || p.speed > 100 || !std::isfinite(p.weight) ||
      p.weight < 0 || p.weight > 100)
    return false;
  for (float x : p.user_data)
    if (!std::isfinite(x))
      return false;
  return true;
}
inline bool default_animation_layer(const AnimationLayer& layer) {
  const AnimationLayer empty;
  // Compare all parameter bits, including signed zero. Sequence zero alone does
  // not permit dropping a nondefault clip, token, layer, time, or user parameter.
  return layer.sequence == empty.sequence && layer.clip == empty.clip &&
         std::memcmp(&layer.params, &empty.params, sizeof(AnimationParams)) == 0;
}

enum class AnimationEncoding { LayerSparse, ScalarSparse };

inline std::array<std::uint32_t, 16> animation_parameter_words(const AnimationParams& params) {
  std::array<std::uint32_t, 16> words;
  std::memcpy(words.data(), &params, sizeof(params));
  return words;
}

// Versioned little-endian payload. KSA2 omits exact default layers. KSA3 additionally
// omits parameter words whose 32 bits equal the default, with a u16 word mask per
// present layer. Sequence and clip keep their original 12-byte layout. Signed zero,
// unknown unsequenced parameters and stop sequences remain bit-exact. Select the
// smallest format, preferring KSA1/2 on a tie; the BodyState bound stays 1220 bytes.
inline std::string encode_animation(const AnimationState& state,
                                    AnimationEncoding encoding = AnimationEncoding::ScalarSparse) {
  std::uint32_t presence = 0;
  std::array<std::uint16_t, 16> parameter_masks{};
  const auto defaults = animation_parameter_words(AnimationParams{});
  std::size_t scalar_bytes = 8;
  for (unsigned i = 0; i < state.size(); ++i) {
    if (!default_animation_layer(state[i])) {
      presence |= 1u << i;
      if (encoding == AnimationEncoding::ScalarSparse) {
        const auto words = animation_parameter_words(state[i].params);
        for (unsigned j = 0; j < words.size(); ++j) {
          if (words[j] != defaults[j]) {
            parameter_masks[i] |= std::uint16_t(1u << j);
          }
        }
        scalar_bytes += 14 + 4 * std::popcount(parameter_masks[i]);
      }
    }
  }
  const bool sparse = presence != 0xFFFF;
  const std::size_t layer_bytes = sparse ? 8 + 76 * std::popcount(presence) : 1220;
  const bool scalar = encoding == AnimationEncoding::ScalarSparse && scalar_bytes < layer_bytes;
  std::string out = scalar ? "KSA3" : sparse ? "KSA2" : "KSA1";
  out.reserve(scalar ? scalar_bytes : layer_bytes);
  auto word = [&](std::uint32_t x) {
    for (unsigned i = 0; i < 4; ++i)
      out.push_back(char(x >> (8 * i)));
  };
  if (sparse || scalar) {
    word(presence);
  }
  for (unsigned i = 0; i < state.size(); ++i) {
    if ((sparse || scalar) && (presence & (1u << i)) == 0) {
      continue;
    }
    const auto& a = state[i];
    word(std::uint32_t(a.sequence));
    word(std::uint32_t(a.sequence >> 32));
    word(std::uint32_t(a.clip));
    const auto words = animation_parameter_words(a.params);
    if (scalar) {
      out.push_back(char(parameter_masks[i]));
      out.push_back(char(parameter_masks[i] >> 8));
    }
    for (unsigned j = 0; j < words.size(); ++j) {
      if (!scalar || (parameter_masks[i] & (1u << j)) != 0) {
        word(words[j]);
      }
    }
  }
  return out;
}
inline std::optional<AnimationState> decode_animation(const std::string& data) {
  const bool legacy = data.size() == 1220 && data.compare(0, 4, "KSA1") == 0;
  const bool sparse = data.size() >= 8 && data.size() <= 1220 && data.compare(0, 4, "KSA2") == 0;
  const bool scalar = data.size() >= 8 && data.size() <= 1220 && data.compare(0, 4, "KSA3") == 0;
  if (!legacy && !sparse && !scalar) {
    return std::nullopt;
  }
  std::size_t pos = 4;
  auto word = [&]() {
    std::uint32_t x = 0;
    for (unsigned i = 0; i < 4; ++i)
      x |= std::uint32_t(static_cast<unsigned char>(data[pos++])) << (8 * i);
    return x;
  };
  const std::uint32_t presence = legacy ? 0xFFFF : word();
  if ((presence & ~0xFFFFu) != 0 ||
      (!scalar && data.size() != (sparse ? 8u : 4u) + 76u * std::popcount(presence))) {
    return std::nullopt;
  }
  AnimationState state{};
  for (unsigned i = 0; i < state.size(); ++i) {
    if ((presence & (1u << i)) == 0) {
      continue;
    }
    if (data.size() - pos < (scalar ? 14u : 76u)) {
      return std::nullopt;
    }
    auto& a = state[i];
    const auto lo = word(), hi = word();
    a.sequence = lo | (std::uint64_t(hi) << 32);
    a.clip = std::int32_t(word());
    std::uint16_t mask = 0xFFFF;
    if (scalar) {
      mask = std::uint16_t(static_cast<unsigned char>(data[pos]) |
                           (unsigned(static_cast<unsigned char>(data[pos + 1])) << 8));
      pos += 2;
      if (data.size() - pos < 4u * std::popcount(mask)) {
        return std::nullopt;
      }
    }
    auto words = animation_parameter_words(AnimationParams{});
    for (unsigned j = 0; j < words.size(); ++j) {
      if ((mask & (1u << j)) != 0) {
        words[j] = word();
      }
    }
    std::memcpy(&a.params, words.data(), sizeof(a.params));
    if (!valid_animation(a, i))
      return std::nullopt;
  }
  if (pos != data.size()) {
    return std::nullopt;
  }
  return state;
}
}  // namespace kcd2s::local
