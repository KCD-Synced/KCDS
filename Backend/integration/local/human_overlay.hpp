#pragma once

#include "animation.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace kcd2s::local {

// This is the observed human overlay layer, not a universal native facial-layer enum.
// Native asset identity is an alias hash resolved in the receiver's current animation set.
constexpr unsigned kHumanOverlayLayer = 11;
constexpr std::size_t kHumanOverlayWireBytes = 88;

enum class HumanOverlayState : std::uint8_t {
  Unset = 0,
  Stop = 1,
  Start = 2,
};

struct HumanOverlay {
  std::uint64_t sequence = 0;
  std::uint64_t alias_hash = 0;
  HumanOverlayState state = HumanOverlayState::Unset;
  std::uint8_t asset_type = 0;
  AnimationParams params{.layer = kHumanOverlayLayer};
};

inline bool valid_human_overlay(const HumanOverlay& overlay) {
  if (overlay.asset_type != 0) {
    return false;
  }
  switch (overlay.state) {
    case HumanOverlayState::Unset:
      if (overlay.sequence != 0 || overlay.alias_hash != 0) {
        return false;
      }
      break;
    case HumanOverlayState::Stop:
      if (overlay.sequence == 0 || overlay.alias_hash != 0) {
        return false;
      }
      break;
    case HumanOverlayState::Start:
      if (overlay.sequence == 0 || overlay.alias_hash == 0) {
        return false;
      }
      break;
    default:
      return false;
  }

  // A nonzero dummy sequence makes the existing parameter validator run for every state,
  // including Unset and Stop. No native clip ID is part of this field.
  AnimationLayer checked;
  checked.sequence = 1;
  checked.clip = 0;
  checked.params = overlay.params;
  return valid_animation(checked, kHumanOverlayLayer);
}

// Invalid local state produces no field. All words are explicit little endian scalars;
// neither native padding nor a process-local clip index is serialized.
inline std::string encode_human_overlay(const HumanOverlay& overlay) {
  if (!valid_human_overlay(overlay)) {
    return {};
  }
  std::string out = "KFA1";
  out.reserve(kHumanOverlayWireBytes);
  auto word = [&](std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
      out.push_back(char(value >> (8 * i)));
    }
  };
  auto wide = [&](std::uint64_t value) {
    word(std::uint32_t(value));
    word(std::uint32_t(value >> 32));
  };
  auto scalar = [&](float value) { word(std::bit_cast<std::uint32_t>(value)); };

  wide(overlay.sequence);
  wide(overlay.alias_hash);
  out.push_back(char(overlay.state));
  out.push_back(char(overlay.asset_type));
  out.push_back(0);
  out.push_back(0);

  const auto& params = overlay.params;
  scalar(params.transition);
  scalar(params.normalized_time);
  scalar(params.speed);
  scalar(params.weight);
  word(params.layer);
  word(params.token);
  word(params.flags);
  word(params.user_token);
  for (float value : params.user_data) {
    scalar(value);
  }
  return out;
}

inline std::optional<HumanOverlay> decode_human_overlay(const std::string& data) {
  if (data.size() != kHumanOverlayWireBytes || data.compare(0, 4, "KFA1") != 0 || data[22] != 0 ||
      data[23] != 0) {
    return std::nullopt;
  }
  std::size_t position = 4;
  auto word = [&]() {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      value |= std::uint32_t(static_cast<unsigned char>(data[position++])) << (8 * i);
    }
    return value;
  };
  auto wide = [&]() {
    const auto low = word();
    const auto high = word();
    return std::uint64_t(low) | (std::uint64_t(high) << 32);
  };
  auto scalar = [&]() { return std::bit_cast<float>(word()); };

  HumanOverlay overlay;
  overlay.sequence = wide();
  overlay.alias_hash = wide();
  overlay.state = static_cast<HumanOverlayState>(static_cast<unsigned char>(data[position++]));
  overlay.asset_type = static_cast<std::uint8_t>(static_cast<unsigned char>(data[position++]));
  position += 2;

  auto& params = overlay.params;
  params.transition = scalar();
  params.normalized_time = scalar();
  params.speed = scalar();
  params.weight = scalar();
  params.layer = word();
  params.token = word();
  params.flags = word();
  params.user_token = word();
  for (float& value : params.user_data) {
    value = scalar();
  }
  if (!valid_human_overlay(overlay)) {
    return std::nullopt;
  }
  return overlay;
}

}  // namespace kcd2s::local
