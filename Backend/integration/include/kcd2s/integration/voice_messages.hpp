#pragma once
// Voice chat's messages on the game's control channel. The frames themselves are
// datagrams of their own (kcd2s/voice/frame.hpp).
//
//   VOICE_STATE : server -> a player's client: [tag][u8 enabled][u8 tier][u8 muted][u8 deaf]
//                 [u32 session][32 server public key][3 x f32 tier ranges][u32 voice ip][u16 voice
//                 port][u8 indicator]. Sent when the player is ready and when any of it changes.
//                 Enabled 0: the client closes its devices. Voice ip 0: voice goes to the game
//                 server's own address and port; otherwise to a standalone voice server there,
//                 both in network order. Indicator 0: the client draws no voice indicator of its
//                 own (Voice.setIndicator); a state without the byte (an older server)
//                 reads as 1.
//   VOICE_KEY   : client -> server: [tag][32 client public key], answering the first state.
//   VOICE_TIER  : client -> server: [tag][u8 tier], the player asks for another tier.
//   VOICE_MUFFLE: server -> client: [tag][u32 talker][u8 amount 0..255], how muffled a talker
//                 sounds to everyone (a gag); sent on change and to a player who joins.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace kcd2s::integration {

inline constexpr std::uint8_t kVoiceStateTag = 230;
inline constexpr std::uint8_t kVoiceKeyTag = 231;
inline constexpr std::uint8_t kVoiceTierTag = 232;
inline constexpr std::uint8_t kVoiceMuffleTag = 233;
inline constexpr std::uint8_t kVoiceTierCount = 3;
inline constexpr std::size_t kVoiceStateBytesWithoutIndicator = 1 + 4 + 4 + 32 + 12 + 4 + 2;
inline constexpr std::size_t kVoiceStateBytes = kVoiceStateBytesWithoutIndicator + 1;

struct VoiceState {
  bool enabled = true;
  std::uint8_t tier = 1;
  bool muted = false;
  bool deaf = false;
  std::uint32_t session = 0;
  std::array<std::uint8_t, 32> server_key{};
  std::array<float, 3> ranges{2.0f, 10.0f, 30.0f};
  std::uint32_t voice_ip = 0;
  std::uint16_t voice_port = 0;
  bool indicator = true;
  friend bool operator==(const VoiceState& a, const VoiceState& b) {
    return a.enabled == b.enabled && a.tier == b.tier && a.muted == b.muted && a.deaf == b.deaf &&
           a.session == b.session && a.server_key == b.server_key && a.ranges == b.ranges &&
           a.voice_ip == b.voice_ip && a.voice_port == b.voice_port && a.indicator == b.indicator;
  }
};

inline std::vector<std::uint8_t> encode_voice_state(const VoiceState& state) {
  std::vector<std::uint8_t> out(kVoiceStateBytes);
  out[0] = kVoiceStateTag;
  out[1] = state.enabled ? 1 : 0;
  out[2] = state.tier;
  out[3] = state.muted ? 1 : 0;
  out[4] = state.deaf ? 1 : 0;
  std::memcpy(out.data() + 5, &state.session, 4);
  std::memcpy(out.data() + 9, state.server_key.data(), 32);
  std::memcpy(out.data() + 41, state.ranges.data(), 12);
  std::memcpy(out.data() + 53, &state.voice_ip, 4);
  std::memcpy(out.data() + 57, &state.voice_port, 2);
  out[59] = state.indicator ? 1 : 0;
  return out;
}

inline bool decode_voice_state(const std::vector<std::uint8_t>& in, VoiceState& state) {
  const bool sized = in.size() == kVoiceStateBytes || in.size() == kVoiceStateBytesWithoutIndicator;
  if (!sized || in[0] != kVoiceStateTag || in[2] >= kVoiceTierCount) {
    return false;
  }
  state.indicator = in.size() == kVoiceStateBytesWithoutIndicator || in[59] != 0;
  state.enabled = in[1] != 0;
  state.tier = in[2];
  state.muted = in[3] != 0;
  state.deaf = in[4] != 0;
  std::memcpy(&state.session, in.data() + 5, 4);
  std::memcpy(state.server_key.data(), in.data() + 9, 32);
  std::memcpy(state.ranges.data(), in.data() + 41, 12);
  std::memcpy(&state.voice_ip, in.data() + 53, 4);
  std::memcpy(&state.voice_port, in.data() + 57, 2);
  return true;
}

inline std::vector<std::uint8_t> encode_voice_key(const std::array<std::uint8_t, 32>& key) {
  std::vector<std::uint8_t> out(33);
  out[0] = kVoiceKeyTag;
  std::memcpy(out.data() + 1, key.data(), 32);
  return out;
}

inline bool decode_voice_key(const std::vector<std::uint8_t>& in,
                             std::array<std::uint8_t, 32>& key) {
  if (in.size() != 33 || in[0] != kVoiceKeyTag) {
    return false;
  }
  std::memcpy(key.data(), in.data() + 1, 32);
  return true;
}

inline std::vector<std::uint8_t> encode_voice_tier(std::uint8_t tier) {
  return {kVoiceTierTag, tier};
}

inline bool decode_voice_tier(const std::vector<std::uint8_t>& in, std::uint8_t& tier) {
  if (in.size() != 2 || in[0] != kVoiceTierTag || in[1] >= kVoiceTierCount) {
    return false;
  }
  tier = in[1];
  return true;
}

inline std::vector<std::uint8_t> encode_voice_muffle(std::uint32_t talker, std::uint8_t amount) {
  std::vector<std::uint8_t> out(6);
  out[0] = kVoiceMuffleTag;
  std::memcpy(out.data() + 1, &talker, 4);
  out[5] = amount;
  return out;
}

inline bool decode_voice_muffle(const std::vector<std::uint8_t>& in, std::uint32_t& talker,
                                std::uint8_t& amount) {
  if (in.size() != 6 || in[0] != kVoiceMuffleTag) {
    return false;
  }
  std::memcpy(&talker, in.data() + 1, 4);
  amount = in[5];
  return true;
}

}  // namespace kcd2s::integration
